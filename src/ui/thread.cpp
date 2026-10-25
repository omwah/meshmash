#include "ui/thread.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace ui {

bool firstMention(const char* text, char* out, int n) {
  const char* p = strstr(text, "@[");
  if (!p) return false;
  p += 2;
  const char* e = strchr(p, ']');
  if (!e || e == p) return false;
  int len = (int)(e - p);
  if (len >= n) len = n - 1;
  memcpy(out, p, len);
  out[len] = 0;
  return true;
}

bool nextMention(const char* text, const char** pos, char* out, int n) {
  const char* p = strstr(*pos, "@[");
  if (!p) return false;
  p += 2;
  const char* e = strchr(p, ']');
  if (!e) return false;
  *pos = e + 1;
  if (e == p) return nextMention(text, pos, out, n);   // an empty @[] names no one
  int len = (int)(e - p);
  if (len >= n) len = n - 1;
  memcpy(out, p, len);
  out[len] = 0;
  return true;
}

namespace {

// ---- features of one message

constexpr int MAX_TARGETS = 3;   // names a message addresses (more are ignored)
constexpr int MAX_TOKENS = 10;   // content words kept per message

// Weights: strong evidence is 6 and up, weak 3 to 5.
constexpr int W_MENTION = 10;        // @[X]: the last thing X said
constexpr int W_BURST = 8;           // X's messages in a row ending there: a mention covers the whole burst
constexpr int W_ADDRESSED = 7;       // a recent message named the sender: this is the answer
constexpr int W_SELF = 6;            // the sender's own previous message, right before (or sharing words)
constexpr int W_DIALOG = 4;          // back and forth between two people a mention already linked
constexpr int W_TOPIC = 3;           // shared words
constexpr int STRONG = 6;
constexpr int EXPLICIT = 8;          // evidence that may join two conversations
constexpr int LOOKBACK = 8;          // implicit evidence reaches this far back, in messages
constexpr int ADDRESSED_BACK = 6;
constexpr uint32_t IMPLICIT_SECONDS = 20 * 60;   // and this far in time, when times are usable

struct Feat {
  uint32_t who;                     // sender (a hash of the name; ours for outgoing)
  uint32_t target[MAX_TARGETS];     // names it addresses
  uint8_t nTargets;
  uint16_t token[MAX_TOKENS];
  uint8_t nTokens;
  uint32_t ts;
};

struct Edge {
  int16_t from, to;                 // from answers to (to < from)
  uint8_t weight;
  uint8_t explicitMention;
  uint8_t bridges;                  // direct enough to join two conversations (a mention, a reply to a mention)
};

// A name as a key. People decorate their names with emoji, pad them, and the saved sender is cut at
// 23 bytes where a mention carries the whole name, so the key is made of what survives all that:
// the first 12 letters and digits, lower case (@, spaces, emoji, punctuation left out). A name with
// none (all emoji, or not in Latin letters) falls back to its bytes without spaces, variation
// selectors and joiners.
uint32_t nameKey(const char* s) {
  uint32_t h = 2166136261u;
  int kept = 0;
  for (const unsigned char* p = (const unsigned char*)s; *p && kept < 12; p++) {
    unsigned char c = *p;
    if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) { h = (h ^ c) * 16777619u; kept++; }
  }
  if (kept) return h | 1;
  int n = 0;
  h = 2166136261u;
  for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
    if (*p == ' ' || *p == '@') continue;
    if (p[0] == 0xEF && p[1] == 0xB8 && p[2] == 0x8F) { p += 2; continue; }   // variation selector-16
    if (p[0] == 0xE2 && p[1] == 0x80 && p[2] == 0x8D) { p += 2; continue; }   // zero width joiner
    h = (h ^ *p) * 16777619u;
    n++;
  }
  return n ? (h | 1) : 0;
}

const char* const kStop[] = {"this", "that", "with", "have", "from", "they", "what", "your", "just", "like", "will",
                             "there", "about", "would", "been", "were", "when", "then", "than", "them", "here",
                             "good", "know", "yeah", "thanks", "thank", "also", "some", "more", "very", "does",
                             "dont", "cant", "its", "much", "only", "into", "over", "back", "now", "well"};

bool stopWord(const char* w) {
  for (const char* s : kStop)
    if (!strcmp(w, s)) return true;
  return false;
}

// Content words of the text (not the @[...] parts): lower case letters and digits, 4 or more.
void tokens(const char* text, Feat& f) {
  f.nTokens = 0;
  char w[16];
  int n = 0;
  for (const char* p = text;; p++) {
    char c = *p;
    if (c == '@' && p[1] == '[') {   // a mention: skip it
      const char* e = strchr(p, ']');
      if (e) { p = e; continue; }
    }
    bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    if (alnum) {
      if (n < (int)sizeof w - 1) w[n++] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
      else n = (int)sizeof w;   // too long to be a plain word: dropped at its end
    } else {
      if (n >= 4 && n < (int)sizeof w) {
        w[n] = 0;
        if (!stopWord(w)) {
          uint16_t h = 5381;
          for (int i = 0; i < n; i++) h = (uint16_t)(h * 33 + (unsigned char)w[i]);
          bool dup = false;
          for (int i = 0; i < f.nTokens; i++) dup = dup || f.token[i] == h;
          if (!dup && f.nTokens < MAX_TOKENS) f.token[f.nTokens++] = h;
        }
      }
      n = 0;
    }
    if (!c) break;
  }
}

int shared(const Feat& a, const Feat& b) {
  int k = 0;
  for (int i = 0; i < a.nTokens; i++)
    for (int j = 0; j < b.nTokens; j++)
      if (a.token[i] == b.token[j]) { k++; break; }
  return k;
}

bool addresses(const Feat& f, uint32_t who) {
  for (int i = 0; i < f.nTargets; i++)
    if (f.target[i] == who) return true;
  return false;
}

int find(int16_t* up, int x) {
  while (up[x] != x) { up[x] = up[up[x]]; x = up[x]; }
  return x;
}

}  // namespace

int buildThreadEx(MessageAt at, void* ctx, int count, int idx, const char* me, int* out, uint8_t* conf, int max) {
  if (idx < 0 || idx >= count || max < 1) return 0;
  if (max > THREAD_MAX) max = THREAD_MAX;
  // The window of messages looked at: those around the picked one.
  int lo = 0, n = count;
  if (n > THREAD_WINDOW) {
    lo = idx - THREAD_WINDOW * 3 / 4;
    if (lo < 0) lo = 0;
    if (lo > count - THREAD_WINDOW) lo = count - THREAD_WINDOW;
    n = THREAD_WINDOW;
  }
  const int pick = idx - lo;
  Feat* f = (Feat*)calloc(n, sizeof(Feat));
  Edge* edges = (Edge*)malloc(sizeof(Edge) * n * 6);
  if (!f || !edges) {
    free(f);
    free(edges);
    out[0] = idx;
    if (conf) conf[0] = 0;
    return 1;
  }
  const uint32_t meKey = nameKey(me);
  int nEdges = 0;
  const int maxEdges = n * 6;

  model::Message m;
  for (int i = 0; i < n; i++) {
    if (!at(ctx, lo + i, m)) continue;
    Feat& x = f[i];
    x.who = m.outgoing ? meKey : nameKey(m.sender);
    x.ts = m.ts;
    const char* pos = m.text;
    char name[model::NAME_LEN];
    while (x.nTargets < MAX_TARGETS && nextMention(m.text, &pos, name, sizeof name)) {
      uint32_t k = nameKey(name);
      bool dup = k == 0 || k == x.who;   // a message that names its own author names no one else
      for (int t = 0; t < x.nTargets; t++) dup = dup || x.target[t] == k;
      if (!dup) x.target[x.nTargets++] = k;
    }
    tokens(m.text, x);
  }
  // Times are usable when they never run backwards by more than a minute around the pair.
  auto gap = [&](int i, int j) -> int64_t {   // seconds from i to j, or -1 if unknown
    if (!f[i].ts || !f[j].ts || f[j].ts + 60 < f[i].ts) return -1;
    return f[j].ts > f[i].ts ? (int64_t)(f[j].ts - f[i].ts) : 0;
  };
  auto addEdge = [&](int from, int to, int w, bool mention, bool bridges = false) {
    if (nEdges < maxEdges) edges[nEdges++] = Edge{(int16_t)from, (int16_t)to, (uint8_t)w, (uint8_t)mention, (uint8_t)(mention || bridges)};
  };
  // Pairs of people a mention has linked, for the back-and-forth rule.
  uint32_t pairA[48], pairB[48];
  int nPairs = 0;
  auto linked = [&](uint32_t a, uint32_t b) {
    for (int i = 0; i < nPairs; i++)
      if ((pairA[i] == a && pairB[i] == b) || (pairA[i] == b && pairB[i] == a)) return true;
    return false;
  };

  for (int j = 0; j < n; j++) {
    const Feat& x = f[j];
    if (!x.who) continue;
    bool strongSoFar = false;
    // Mentions: each one is resolved on its own, to the last thing that person said and the burst it ended.
    for (int t = 0; t < x.nTargets; t++) {
      int i = j - 1;
      while (i >= 0 && f[i].who != x.target[t]) i--;
      if (i < 0) continue;
      // Usually the last thing they said. When that shares no words with this message but one of
      // their few before it does, this answers that one (they were talking about two things).
      if (!shared(x, f[i])) {
        int seen = 0;
        for (int c = i - 1; c >= 0 && seen < 3 && i - c <= 40; c--)
          if (f[c].who == x.target[t]) {
            seen++;
            if (shared(x, f[c])) { i = c; break; }
          }
      }
      addEdge(j, i, W_MENTION, true);
      strongSoFar = true;
      // The burst that message ended: what they sent in a row, unless it is about something else.
      for (int k = i - 1, run = 0; k >= 0 && f[k].who == x.target[t] && run < 4; k--, run++) {
        bool otherTopic = f[k].nTokens && f[i].nTokens && !shared(f[k], f[i]) && !(f[k].nTokens && shared(x, f[k]));
        if (otherTopic) break;
        addEdge(j, k, W_BURST, true);
      }
      if (nPairs < 48) { pairA[nPairs] = x.who; pairB[nPairs] = x.target[t]; nPairs++; }
    }
    // Implicit evidence: the best single one, if it is recent enough.
    int bestW = 0, bestTo = -1;
    bool bestBridges = false;
    auto consider = [&](int i, int w, bool bridges = false) {
      int64_t g = gap(i, j);
      if (g > (int64_t)IMPLICIT_SECONDS) return;   // long ago: only a mention reaches that far
      if (g >= 0 && g <= 60) w++;
      w -= (j - i - 1) / 4;
      if (w > bestW) { bestW = w; bestTo = i; bestBridges = bridges; }
    };
    // The sender's own previous message: a continuation, when it comes right after, or with others
    // between when they share words. Two in a row that both name someone are one flurry.
    for (int i = j - 1; i >= 0 && j - i <= 4; i--)
      if (f[i].who == x.who) {
        if (i == j - 1 && x.nTargets && f[i].nTargets) addEdge(j, i, W_BURST, true);
        else if (i == j - 1 || shared(x, f[i])) consider(i, W_SELF);
        break;
      }
    if (!x.nTargets) {
      // The answer to someone who just named the sender.
      for (int i = j - 1; i >= 0 && j - i <= ADDRESSED_BACK; i--)
        if (f[i].who != x.who && addresses(f[i], x.who)) { consider(i, W_ADDRESSED, true); break; }
      // Back and forth between two people a mention linked.
      for (int i = j - 1; i >= 0 && j - i <= LOOKBACK; i--)
        if (f[i].who != x.who && linked(f[i].who, x.who)) { consider(i, W_DIALOG); break; }
    }
    // Shared words with a message a few steps back.
    for (int i = j - 1; i >= 0 && j - i <= LOOKBACK; i--) {
      int s = shared(x, f[i]);
      if (s >= 2 && f[i].who != x.who) consider(i, W_TOPIC + (s > 2 ? 1 : 0));
    }
    if (bestTo >= 0 && (bestW >= STRONG || !strongSoFar)) addEdge(j, bestTo, bestW, false, bestBridges);
  }

  // Components over strong evidence. Mentions join conversations; implicit evidence only attaches
  // a message that has no conversation yet.
  int16_t up[THREAD_WINDOW];
  uint8_t size[THREAD_WINDOW];
  for (int i = 0; i < n; i++) { up[i] = (int16_t)i; size[i] = 1; }
  auto unite = [&](int a, int b) {
    a = find(up, a);
    b = find(up, b);
    if (a == b) return;
    if (size[a] < size[b]) { int t = a; a = b; b = t; }
    up[b] = (int16_t)a;
    size[a] = (uint8_t)(size[a] + size[b] > 255 ? 255 : size[a] + size[b]);
  };
  for (int e = 0; e < nEdges; e++)
    if (edges[e].explicitMention) unite(edges[e].from, edges[e].to);
  for (int e = 0; e < nEdges; e++) {
    const Edge& g = edges[e];
    if (g.explicitMention || g.weight < STRONG) continue;
    int a = find(up, g.from), b = find(up, g.to);
    if (a != b && (g.bridges || size[a] == 1 || size[b] == 1)) unite(a, b);
  }

  // A message that no evidence ties to anything, from someone already taking part in a conversation
  // that went on a moment ago: weakly part of it, when only one conversation fits.
  for (int j = 0; j < n; j++) {
    if (!f[j].who || size[find(up, j)] != 1) continue;
    int bestComp = -1, bestTo = -1, bestScore = 0, second = 0;
    for (int i = j - 1; i >= 0 && j - i <= LOOKBACK; i--) {
      int c = find(up, i);
      if (size[c] < 2 || c == bestComp) continue;
      bool takesPart = false;
      for (int k = 0; k < n && !takesPart; k++)
        if (find(up, k) == c) takesPart = f[k].who == f[j].who || addresses(f[k], f[j].who);
      if (!takesPart) continue;
      int64_t g = gap(i, j);
      if (g > (int64_t)IMPLICIT_SECONDS) continue;
      int score = 4 + (shared(f[j], f[i]) ? 1 : 0) + (g >= 0 && g <= 120 ? 1 : 0);
      if (score > bestScore) { second = bestScore; bestScore = score; bestComp = c; bestTo = i; }
      else if (score > second) second = score;
    }
    if (bestTo >= 0 && bestScore > second) addEdge(j, bestTo, bestScore > 5 ? 5 : bestScore, false);
  }

  // The picked message's conversation, and what hangs on it by weak evidence, one step away.
  const int root = find(up, pick);
  bool* in = (bool*)calloc(n, 1);
  bool* weak = (bool*)calloc(n, 1);
  int* queue = (int*)malloc(sizeof(int) * n);
  int total = 0;
  if (in && weak && queue) {
    for (int i = 0; i < n; i++) in[i] = find(up, i) == root;
    for (int e = 0; e < nEdges; e++) {
      const Edge& g = edges[e];
      if (g.weight >= STRONG) continue;
      int a = g.from, b = g.to;   // a weak edge attaches a message that has no conversation of its own
      if (in[a] && !in[b] && size[find(up, b)] == 1) weak[b] = true;
      else if (in[b] && !in[a] && size[find(up, a)] == 1) weak[a] = true;
    }
    // Nearest to the picked message first, when there are too many for the screen.
    int q = 0, qn = 0;
    bool* seen = (bool*)calloc(n, 1);
    if (seen) {
      queue[qn++] = pick;
      seen[pick] = true;
      while (q < qn) {
        int cur = queue[q++];
        for (int e = 0; e < nEdges; e++) {
          const Edge& g = edges[e];
          int other = g.from == cur ? g.to : g.to == cur ? g.from : -1;
          if (other < 0 || seen[other] || !(in[other] || weak[other])) continue;
          if (g.weight < STRONG && !(weak[other] || weak[cur])) continue;
          seen[other] = true;
          queue[qn++] = other;
        }
      }
      // Whatever the walk missed (joined through the other end of an edge) comes after, by distance.
      for (int d = 1; d < n; d++)
        for (int s = -1; s <= 1; s += 2) {
          int i = pick + s * d;
          if (i >= 0 && i < n && !seen[i] && (in[i] || weak[i])) { seen[i] = true; queue[qn++] = i; }
        }
      free(seen);
    } else {
      queue[qn++] = pick;
    }
    // Keep the first `max`, in order of index.
    bool* keep = (bool*)calloc(n, 1);
    if (keep) {
      for (int k = 0; k < qn && k < max; k++) keep[queue[k]] = true;
      for (int i = 0; i < n; i++)
        if (keep[i]) {
          if (conf) conf[total] = (uint8_t)(in[i] ? 0 : 1);
          out[total++] = lo + i;
        }
      free(keep);
    }
  }
  free(in);
  free(weak);
  free(queue);
  free(f);
  free(edges);
  if (!total) {
    out[0] = idx;
    if (conf) conf[0] = 0;
    return 1;
  }
  return total;
}

int buildThread(MessageAt at, void* ctx, int count, int idx, const char* me, int* out, int max) {
  return buildThreadEx(at, ctx, count, idx, me, out, nullptr, max);
}

}  // namespace ui
