#include "ui/thread.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

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

// The name without the spaces around it or a leading @: names are shown padded, and a few
// people start theirs with an @, so a mention and a sender may differ in just that.
void trimmed(const char* s, const char** b, int* len) {
  while (*s == ' ' || *s == '@') s++;
  int n = (int)strlen(s);
  while (n > 0 && s[n - 1] == ' ') n--;
  *b = s;
  *len = n;
}

bool sameName(const char* a, const char* b) {
  const char *pa, *pb;
  int la, lb;
  trimmed(a, &pa, &la);
  trimmed(b, &pb, &lb);
  return la && la == lb && strncasecmp(pa, pb, la) == 0;
}

// Who wrote it: outgoing messages carry no name.
bool writtenBy(const model::Message& m, const char* name, const char* me) {
  return sameName(m.outgoing ? me : m.sender, name);
}

bool has(const int* v, int n, int x) {
  for (int i = 0; i < n; i++)
    if (v[i] == x) return true;
  return false;
}

}  // namespace

int buildThread(MessageAt at, void* ctx, int count, int idx, const char* me, int* out, int max) {
  if (idx < 0 || idx >= count || max < 1) return 0;
  if (max > THREAD_MAX) max = THREAD_MAX;
  int set[THREAD_MAX];
  int n = 0;
  int queue[THREAD_MAX];
  int nq = 0;
  auto add = [&](int i) {
    if (has(set, n, i)) return true;
    if (n == max) return false;
    set[n++] = i;
    if (nq < THREAD_MAX) queue[nq++] = i;
    return true;
  };
  model::Message m;
  add(idx);
  // Messages in a row that mention the same people are one conversation, whoever wrote them:
  // take the neighbours of the picked one, before and after, for as long as that holds. What
  // the same author wrote in a row, each mentioning someone, belongs with it too. After the
  // picked message only replies count: those that mention one of its names or its author.
  if (at(ctx, idx, m)) {
    char who[model::NAME_LEN], targets[4][model::NAME_LEN];
    int nt = 0;
    snprintf(who, sizeof who, "%s", m.outgoing ? me : m.sender);
    const char* pos = m.text;
    while (nt < 4 && nextMention(m.text, &pos, targets[nt], sizeof targets[nt])) nt++;
    auto mentionsTarget = [&](const model::Message& x, bool andAuthor) {
      char name[model::NAME_LEN];
      const char* p2 = x.text;
      while (nextMention(x.text, &p2, name, sizeof name)) {
        for (int t = 0; t < nt; t++)
          if (sameName(name, targets[t])) return true;
        if (andAuthor && sameName(name, who)) return true;
      }
      return false;
    };
    auto hasMention = [&](const model::Message& x) {
      char name[model::NAME_LEN];
      const char* p2 = x.text;
      return nextMention(x.text, &p2, name, sizeof name);
    };
    for (int i = idx - 1; i >= 0 && at(ctx, i, m); i--) {
      bool same = nt && (mentionsTarget(m, false) || (writtenBy(m, who, me) && hasMention(m)));
      if (!same || !add(i)) break;
    }
    for (int i = idx + 1; i < count && at(ctx, i, m); i++) {
      bool same = mentionsTarget(m, true) || (nt && writtenBy(m, who, me) && hasMention(m));
      if (!same || !add(i)) break;
    }
  }
  // Each message answers the last thing everyone it mentions wrote, and what they sent just
  // before it; those answer others in turn.
  for (int q = 0; q < nq && q < THREAD_DEPTH * 2; q++) {
    int cur = queue[q];
    if (!at(ctx, cur, m)) continue;
    model::Message cm = m;
    const char* pos = cm.text;
    char name[model::NAME_LEN];
    while (nextMention(cm.text, &pos, name, sizeof name)) {
      if (writtenBy(cm, name, me)) continue;   // a message that names its own author
      int j = cur - 1;
      for (; j >= 0; j--)
        if (at(ctx, j, m) && writtenBy(m, name, me)) break;
      if (j < 0) continue;
      int k = j;
      while (k > 0 && at(ctx, k - 1, m) && writtenBy(m, name, me)) k--;
      for (int i = j; i >= k; i--)
        if (!add(i)) break;
    }
  }
  // Oldest first.
  for (int i = 1; i < n; i++)
    for (int j = i; j > 0 && set[j - 1] > set[j]; j--) { int t = set[j]; set[j] = set[j - 1]; set[j - 1] = t; }
  for (int i = 0; i < n; i++) out[i] = set[i];
  return n;
}

}  // namespace ui
