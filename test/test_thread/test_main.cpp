// Threads found by following mentions back.
#include <unity.h>
#include <algorithm>
#include <initializer_list>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <string>
#include <vector>
#include "ui/thread.h"

using namespace ui;

namespace {

struct Msg { const char* who; const char* text; };   // "me" writes outgoing messages

struct Chat {
  Msg m[16];
  int n = 0;
  void add(const char* who, const char* text) { m[n++] = {who, text}; }
};

bool at(void* ctx, int i, model::Message& out) {
  Chat* c = (Chat*)ctx;
  if (i < 0 || i >= c->n) return false;
  memset(&out, 0, sizeof out);
  out.outgoing = !strcmp(c->m[i].who, "me");
  if (!out.outgoing) snprintf(out.sender, sizeof out.sender, "%s", c->m[i].who);
  snprintf(out.text, sizeof out.text, "%s", c->m[i].text);
  return true;
}

int thread(Chat& c, int idx, int* out) { return buildThread(at, &c, c.n, idx, "me", out, 16); }

}  // namespace

void setUp() {}
void tearDown() {}

void test_first_mention() {
  char n[32];
  TEST_ASSERT_TRUE(firstMention("hi @[Rose01] and @[Bob]", n, sizeof n));
  TEST_ASSERT_EQUAL_STRING("Rose01", n);
  TEST_ASSERT_TRUE(firstMention("@[News Junkie Portable] ok", n, sizeof n));
  TEST_ASSERT_EQUAL_STRING("News Junkie Portable", n);
  TEST_ASSERT_FALSE(firstMention("plain [brackets] and a@b.c", n, sizeof n));
  TEST_ASSERT_FALSE(firstMention("empty @[] name", n, sizeof n));
  TEST_ASSERT_FALSE(firstMention("unclosed @[Bob", n, sizeof n));
}

void test_no_mention_is_just_the_message() {
  Chat c;
  c.add("Alice", "hello");
  c.add("Bob", "hi");
  int t[16];
  TEST_ASSERT_EQUAL(1, thread(c, 1, t));
  TEST_ASSERT_EQUAL(1, t[0]);
}

void test_reply_pulls_in_the_last_run_by_that_person() {
  Chat c;
  c.add("Alice", "old news");          // 0
  c.add("Bob", "something else");      // 1
  c.add("Alice", "who is up?");        // 2
  c.add("Alice", "anyone at all?");    // 3  (a run with 2)
  c.add("Carol", "noise");             // 4
  c.add("Bob", "@[Alice] me");         // 5
  int t[16];
  TEST_ASSERT_EQUAL(3, thread(c, 5, t));
  TEST_ASSERT_EQUAL(2, t[0]);
  TEST_ASSERT_EQUAL(3, t[1]);
  TEST_ASSERT_EQUAL(5, t[2]);
}

void test_run_stops_at_someone_else() {
  Chat c;
  c.add("Alice", "first");             // 0
  c.add("Bob", "in between");          // 1
  c.add("Alice", "second");            // 2
  c.add("Carol", "@[Alice] yes");      // 3
  int t[16];
  TEST_ASSERT_EQUAL(2, thread(c, 3, t));
  TEST_ASSERT_EQUAL(2, t[0]);
  TEST_ASSERT_EQUAL(3, t[1]);
}

void test_follows_a_chain_of_replies() {
  Chat c;
  c.add("Carol", "anyone for lunch?");     // 0
  c.add("Bob", "@[Carol] where?");         // 1
  c.add("Alice", "unrelated");             // 2
  c.add("Carol", "@[Bob] the cafe");       // 3
  c.add("Bob", "@[Carol] on my way");      // 4
  int t[16];
  // 4 answers Carol's 3, which answers Bob's 1 (Bob's last before it), which answers Carol's 0.
  int n = thread(c, 4, t);
  TEST_ASSERT_EQUAL(4, n);
  TEST_ASSERT_EQUAL(0, t[0]);
  TEST_ASSERT_EQUAL(1, t[1]);
  TEST_ASSERT_EQUAL(3, t[2]);
  TEST_ASSERT_EQUAL(4, t[3]);
}

void test_names_match_without_case_and_outgoing_is_me() {
  Chat c;
  c.add("me", "anyone here?");             // 0
  c.add("alice", "@[ME] yes");             // 1
  c.add("Bob", "@[Alice] hello");          // 2
  int t[16];
  TEST_ASSERT_EQUAL(3, thread(c, 2, t));
  TEST_ASSERT_EQUAL(0, t[0]);
  TEST_ASSERT_EQUAL(1, t[1]);
  TEST_ASSERT_EQUAL(2, t[2]);
}

void test_unknown_person_and_loops_end() {
  Chat c;
  c.add("Bob", "@[Zed] hey");              // 0
  c.add("Zed", "@[Bob] what");             // 1
  c.add("Bob", "@[Zed] nothing");          // 2
  int t[16];
  TEST_ASSERT_EQUAL(3, thread(c, 0, t));   // nobody before it, but the replies that mention Bob follow
  int n = thread(c, 2, t);                 // 2 -> 1 -> 0, and no further
  TEST_ASSERT_EQUAL(3, n);
  TEST_ASSERT_EQUAL(0, t[0]);
}

void test_the_limit_is_kept() {
  Chat c;
  for (int i = 0; i < 8; i++) { c.add("A", "@[B] x"); c.add("B", "@[A] y"); }
  int t[4];
  int n = buildThread(at, &c, c.n, 15, "me", t, 4);
  TEST_ASSERT_TRUE(n >= 1 && n <= 4);
  for (int i = 1; i < n; i++) TEST_ASSERT_TRUE(t[i - 1] < t[i]);
}

void test_padded_and_at_names_match() {
  Chat c;
  c.add("Alice Mobile ", "hello");         // 0  senders are shown padded
  c.add("@Bobby", "hi there");             // 1  and some start with an @
  c.add("Carol", "@[Alice Mobile] ok");    // 2
  c.add("Dave", "@[@Bobby] you too");      // 3
  c.add("Erin", "@[Bobby] again");         // 4
  int t[16];
  TEST_ASSERT_EQUAL(2, thread(c, 2, t));
  TEST_ASSERT_EQUAL(0, t[0]);
  TEST_ASSERT_EQUAL(3, thread(c, 3, t));   // Bobby's message, and both that mention him
  TEST_ASSERT_EQUAL(1, t[0]);
  TEST_ASSERT_EQUAL(3, thread(c, 4, t));
  TEST_ASSERT_EQUAL(1, t[0]);
}

void test_several_mentions_in_one_message_are_all_followed() {
  Chat c;
  c.add("Alice", "lunch?");                  // 0
  c.add("Bob", "noise");                     // 1
  c.add("Carol", "what time?");              // 2
  c.add("Dave", "@[Alice] @[Carol] noon");   // 3
  int t[16];
  TEST_ASSERT_EQUAL(3, thread(c, 3, t));
  TEST_ASSERT_EQUAL(0, t[0]);
  TEST_ASSERT_EQUAL(2, t[1]);
  TEST_ASSERT_EQUAL(3, t[2]);
}

void test_mentions_in_a_row_are_taken_together() {
  Chat c;
  c.add("Alice", "anyone?");                 // 0
  c.add("Bob", "here");                      // 1
  c.add("Carol", "@[Alice] hi");             // 2
  c.add("Carol", "@[Bob] and you");          // 3
  c.add("Carol", "@[Alice] again");          // 4
  int t[16];
  int n = thread(c, 4, t);
  TEST_ASSERT_EQUAL(5, n);
  for (int i = 0; i < 5; i++) TEST_ASSERT_EQUAL(i, t[i]);
  // A flurry from one person is one conversation, even with a line in it that names no one.
  Chat d;
  d.add("Alice", "anyone?");                 // 0
  d.add("Carol", "@[Alice] hi");             // 1
  d.add("Carol", "just talking");            // 2
  d.add("Carol", "@[Alice] again");          // 3
  TEST_ASSERT_EQUAL(4, thread(d, 3, t));
}

void test_mentions_in_a_row_by_different_people_belong_together() {
  Chat c;
  c.add("Egg", "I am an egg");                // 0
  c.add("Alice", "@[egg] had you for breakfast");   // 1
  c.add("Bob", "@[Egg] I like you over easy");      // 2
  c.add("Carol", "@[egg] watch out");               // 3
  c.add("Dave", "something else");                  // 4
  int t[16];
  // From any of the three: all of them and the egg.
  for (int pick = 1; pick <= 3; pick++) {
    TEST_ASSERT_EQUAL(4, thread(c, pick, t));
    for (int i = 0; i < 4; i++) TEST_ASSERT_EQUAL(i, t[i]);
  }
  // From the egg itself: the replies after it that mention it.
  TEST_ASSERT_EQUAL(4, thread(c, 0, t));
  TEST_ASSERT_EQUAL(3, t[3]);
}

void test_a_message_about_someone_else_stays_out() {
  Chat c;
  c.add("Egg", "hello");                     // 0
  c.add("Alice", "@[Egg] hi");               // 1
  c.add("Bob", "@[Zed] unrelated mention");  // 2
  c.add("Carol", "@[Egg] hey");              // 3
  int t[16];
  // Everyone who answered the egg is in, from either of them; Bob, talking to Zed, is not.
  for (int pick : {1, 3}) {
    TEST_ASSERT_EQUAL(3, thread(c, pick, t));
    TEST_ASSERT_EQUAL(0, t[0]);
    TEST_ASSERT_EQUAL(1, t[1]);
    TEST_ASSERT_EQUAL(3, t[2]);
  }
}

// ---- comparing against the first algorithm (mentions only), on labelled chats

namespace {

// The first version, kept to compare with: it follows mentions only.
bool legacySame(const char* a, const char* b) {
  auto trim = [](const char* s, const char** p, int* n) {
    while (*s == ' ' || *s == '@') s++;
    int len = (int)strlen(s);
    while (len > 0 && s[len - 1] == ' ') len--;
    *p = s;
    *n = len;
  };
  const char *pa, *pb;
  int la, lb;
  trim(a, &pa, &la);
  trim(b, &pb, &lb);
  return la && la == lb && strncasecmp(pa, pb, la) == 0;
}

int legacyThread(MessageAt at, void* ctx, int count, int idx, const char* me, int* out, int max) {
  if (idx < 0 || idx >= count) return 0;
  std::vector<int> set{idx};
  auto by = [&](const model::Message& m, const char* name) { return legacySame(m.outgoing ? me : m.sender, name); };
  auto add = [&](int i) {
    for (int x : set) if (x == i) return true;
    if ((int)set.size() >= max) return false;
    set.push_back(i);
    return true;
  };
  model::Message m;
  size_t q = 0;
  for (int depth = 0; q < set.size() && depth < 16; q++, depth++) {
    int cur = set[q];
    if (!at(ctx, cur, m)) continue;
    model::Message cm = m;
    const char* pos = cm.text;
    char name[model::NAME_LEN];
    while (nextMention(cm.text, &pos, name, sizeof name)) {
      if (by(cm, name)) continue;
      int j = cur - 1;
      for (; j >= 0; j--)
        if (at(ctx, j, m) && by(m, name)) break;
      if (j < 0) continue;
      int k = j;
      while (k > 0 && at(ctx, k - 1, m) && by(m, name)) k--;
      for (int i = j; i >= k; i--) if (!add(i)) break;
    }
  }
  std::sort(set.begin(), set.end());
  for (size_t i = 0; i < set.size(); i++) out[i] = set[i];
  return (int)set.size();
}

struct Row { std::string who, text; int conv; uint32_t ts; };
struct Rows { std::vector<Row> r; };

bool atRows(void* ctx, int i, model::Message& m) {
  Rows* c = (Rows*)ctx;
  if (i < 0 || i >= (int)c->r.size()) return false;
  memset(&m, 0, sizeof m);
  snprintf(m.sender, sizeof m.sender, "%s", c->r[i].who.c_str());
  snprintf(m.text, sizeof m.text, "%s", c->r[i].text.c_str());
  m.ts = c->r[i].ts;
  m.seq = (uint32_t)i + 1;
  return true;
}

struct Lcg {
  uint32_t s;
  uint32_t next() { s = s * 1664525u + 1013904223u; return s >> 8; }
  int pick(int n) { return (int)(next() % (uint32_t)n); }
  bool chance(int percent) { return pick(100) < percent; }
};

// A busy channel: several conversations running at once, each with its own people and topic words,
// people who sometimes name the one they answer and sometimes just answer, and bursts.
Rows makeChat(uint32_t seed, int nMsgs, int nConv) {
  static const char* kPeople[] = {"Anna", "Ben", "Cleo", "Dev", "Eli", "Fay", "Gus", "Hana"};
  static const char* kTopics[][3] = {{"antenna", "mounting", "roof"}, {"battery", "solar", "charging"},
                                     {"firmware", "update", "flashing"}, {"repeater", "hilltop", "coverage"},
                                     {"weather", "forecast", "storm"}};
  static const char* kFiller[] = {"ok", "yes", "agreed", "same here", "good point", "sounds right", "not sure", "later"};
  Lcg g{seed};
  Rows rows;
  struct Conv { std::vector<int> people; int last = -1; int lastSpeaker = -1; bool started = false; };
  std::vector<Conv> conv(nConv);
  for (int c = 0; c < nConv; c++) {
    int n = 2 + g.pick(3);
    for (int i = 0; i < n; i++) {
      int p = (c * 2 + i + g.pick(2)) % 8;
      bool have = false;
      for (int x : conv[c].people) have = have || x == p;
      if (!have) conv[c].people.push_back(p);
    }
  }
  uint32_t ts = 1792900000;
  int cur = 0;
  for (int k = 0; k < nMsgs; k++) {
    if (g.chance(35)) cur = g.pick(nConv);
    Conv& c = conv[cur];
    int speaker;
    std::string text;
    auto topic = [&](int n) {
      for (int i = 0; i < n; i++) text += std::string(text.empty() ? "" : " ") + kTopics[cur % 5][g.pick(3)];
    };
    if (!c.started) {
      c.started = true;
      speaker = c.people[g.pick((int)c.people.size())];
      topic(2);
    } else {
      bool burst = g.chance(15);
      speaker = c.lastSpeaker;
      if (!burst) {
        do speaker = c.people[g.pick((int)c.people.size())]; while (speaker == c.lastSpeaker && c.people.size() > 1);
      }
      if (!burst && g.chance(55)) text = std::string("@[") + kPeople[c.lastSpeaker] + "] ";
      if (g.chance(50)) topic(1 + g.pick(2)); else text += kFiller[g.pick(8)];
    }
    c.lastSpeaker = speaker;
    ts += 15 + g.pick(75);
    rows.r.push_back(Row{kPeople[speaker], text, cur, ts});
  }
  return rows;
}

struct Score { double p = 0, r = 0; int n = 0; double out = 0, truth = 0; };

template <typename F>
Score score(const Rows& rows, F run, int max) {
  Score s;
  int n = (int)rows.r.size();
  std::vector<int> out(max + 4);
  for (int i = 0; i < n; i++) {
    int truth = 0;
    for (const Row& x : rows.r) truth += x.conv == rows.r[i].conv;
    if (truth < 4) continue;
    int got = run(const_cast<Rows*>(&rows), n, i, out.data());
    int tp = 0;
    for (int k = 0; k < got; k++) tp += rows.r[out[k]].conv == rows.r[i].conv;
    s.p += got ? (double)tp / got : 1;
    s.r += (double)tp / (truth < max ? truth : max);
    s.out += got;
    s.truth += truth;
    s.n++;
  }
  return s;
}

}  // namespace

void test_new_algorithm_recalls_more_than_mentions_alone() {
  Score oldS, newS;
  int nChats = 0;
  for (uint32_t seed = 1; seed <= 30; seed++) {
    Rows rows = makeChat(seed, 60, 2 + (int)(seed % 3));
    Score a = score(rows, [](Rows* r, int n, int i, int* out) { return legacyThread(atRows, r, n, i, "me", out, 24); }, 24);
    Score b = score(rows, [](Rows* r, int n, int i, int* out) { return buildThread(atRows, r, n, i, "me", out, 24); }, 24);
    oldS.p += a.p; oldS.r += a.r; oldS.n += a.n;
    newS.p += b.p; newS.r += b.r; newS.n += b.n; newS.out += b.out; newS.truth += b.truth; oldS.out += a.out;
    nChats++;
  }
  printf("\nthreads over %d labelled chats (%d picks), conversations of %.1f messages\n  mentions only: precision %.3f recall %.3f, %.1f messages\n  graph:         precision %.3f recall %.3f, %.1f messages\n",
         nChats, newS.n, newS.truth / newS.n, oldS.p / oldS.n, oldS.r / oldS.n, oldS.out / oldS.n, newS.p / newS.n, newS.r / newS.n, newS.out / newS.n);
  TEST_ASSERT_TRUE(newS.r / newS.n > oldS.r / oldS.n + 0.25);   // finds what mentions alone miss
  TEST_ASSERT_TRUE(newS.p / newS.n >= 0.78);                     // without pulling in other conversations
}

static bool has(const int* v, int n, int x) {
  for (int i = 0; i < n; i++) if (v[i] == x) return true;
  return false;
}

void test_unmentioned_replies_and_continuations_join() {
  Chat c;
  c.add("Anna", "anyone tried the new firmware update?");     // 0
  c.add("Ben", "@[Anna] yes the firmware update went fine");  // 1
  c.add("Anna", "great, flashing mine now");                  // 2  answers Ben, no mention
  c.add("Anna", "will report back");                          // 3  carries on
  int t[16];
  int n = thread(c, 1, t);
  TEST_ASSERT_EQUAL(4, n);
  for (int i = 0; i < 4; i++) TEST_ASSERT_EQUAL(i, t[i]);
  TEST_ASSERT_EQUAL(4, thread(c, 3, t));   // the same conversation from its last message
}

void test_interleaved_conversations_stay_apart() {
  Chat c;
  c.add("Anna", "my roof antenna mounting is loose");         // 0 a
  c.add("Cleo", "battery solar charging is slow today");      // 1 b
  c.add("Ben", "@[Anna] use a bracket for the roof mounting"); // 2 a
  c.add("Dev", "@[Cleo] check the solar panel angle");        // 3 b
  c.add("Anna", "thanks, will try the bracket");              // 4 a
  c.add("Cleo", "@[Dev] angle fixed, charging now");          // 5 b
  int t[16];
  int n = thread(c, 2, t);
  TEST_ASSERT_EQUAL(3, n);
  TEST_ASSERT_TRUE(has(t, n, 0) && has(t, n, 2) && has(t, n, 4));
  n = thread(c, 5, t);
  TEST_ASSERT_EQUAL(3, n);
  TEST_ASSERT_TRUE(has(t, n, 1) && has(t, n, 3) && has(t, n, 5));
}

void test_a_mention_resumes_a_conversation_after_a_break() {
  Chat c;
  c.add("Anna", "the repeater on the hill went quiet");   // 0
  c.add("Ben", "@[Anna] it lost power, I will climb up"); // 1
  for (int i = 0; i < 8; i++) c.add(i % 2 ? "Cleo" : "Dev", i % 2 ? "weather looks fine" : "lunch anyone");   // 2..9
  c.add("Ben", "@[Anna] power is back on the hill");      // 10
  int t[16];
  int n = thread(c, 10, t);
  TEST_ASSERT_TRUE(has(t, n, 0) && has(t, n, 1) && has(t, n, 10));
  TEST_ASSERT_FALSE(has(t, n, 4) || has(t, n, 7));        // the chatter in between stays out
}

void test_someone_in_two_conversations_does_not_join_them() {
  Chat c;
  c.add("Anna", "antenna mounting on the roof?");          // 0 a
  c.add("Anna", "also, battery charging is slow");         // 1 b: Anna starts another topic
  c.add("Ben", "@[Anna] the roof mounting needs a bracket"); // 2 a
  c.add("Cleo", "@[Anna] battery charging needs sun");     // 3 b
  int t[16];
  int n = thread(c, 2, t);
  TEST_ASSERT_TRUE(has(t, n, 0) && has(t, n, 2));
  TEST_ASSERT_FALSE(has(t, n, 3));                         // Cleo's answer is about the battery
}

void test_a_mention_of_someone_not_in_view_and_duplicate_names() {
  Chat c;
  c.add("Anna", "hello everyone");           // 0
  c.add("Ben", "@[Zed] are you there?");     // 1  Zed said nothing in view
  int t[16];
  TEST_ASSERT_EQUAL(1, thread(c, 1, t));     // nothing to follow: the message alone
  Chat d;   // two different people called Sam are one name to us: documented, not guessed at
  d.add("Sam", "radio check");               // 0
  d.add("Sam", "radio check again");         // 1
  d.add("Ana", "@[Sam] loud and clear");     // 2
  int n = thread(d, 2, t);
  TEST_ASSERT_TRUE(has(t, n, 1));
}

void test_the_window_and_its_edges() {
  Rows rows;
  for (int i = 0; i < 300; i++) rows.r.push_back(Row{i % 2 ? "Ben" : "Anna", "filler message here", 0, 1792900000u + (uint32_t)i * 30});
  rows.r[299].text = "@[Anna] still there?";   // Anna's last message is 298
  int t[THREAD_MAX];
  int n = buildThread(atRows, &rows, 300, 299, "me", t, THREAD_MAX);
  TEST_ASSERT_TRUE(n >= 2 && n <= THREAD_MAX);
  for (int i = 1; i < n; i++) TEST_ASSERT_TRUE(t[i - 1] < t[i]);
  TEST_ASSERT_TRUE(t[0] >= 300 - THREAD_WINDOW);          // only what is in the window
  TEST_ASSERT_EQUAL(299, t[n - 1]);
  TEST_ASSERT_EQUAL(1, buildThread(atRows, &rows, 300, 0, "me", t, THREAD_MAX) >= 1);   // the first one, nothing before it
  TEST_ASSERT_EQUAL(0, buildThread(atRows, &rows, 300, 300, "me", t, THREAD_MAX));      // out of range
}

// ---- real conversation shapes (anonymised): how well do mentions survive being hidden?

namespace {

bool loadCorpus(const char* path, int chan, Rows& rows) {
  FILE* f = fopen(path, "r");
  if (!f) return false;
  char line[600];
  while (fgets(line, sizeof line, f)) {
    int c = 0;
    unsigned seq = 0, off = 0;
    int used = 0;
    if (sscanf(line, "%d\t%u\t%u\t%n", &c, &seq, &off, &used) < 3 || c != chan) continue;
    char* who = line + used;
    char* tab = strchr(who, '\t');
    if (!tab) continue;
    *tab = 0;
    std::string text;
    for (char* t = tab + 1; *t && *t != '\n'; t++) {
      if (t[0] == '\\' && t[1] == 'n') { text += '\n'; t++; }
      else text += *t;
    }
    rows.r.push_back(Row{who, text, 0, 1792900000u + off});
  }
  fclose(f);
  return true;
}

// The text without its @[...] mentions.
std::string maskMentions(const std::string& t) {
  std::string out;
  for (size_t i = 0; i < t.size(); i++) {
    if (t[i] == '@' && i + 1 < t.size() && t[i + 1] == '[') {
      size_t e = t.find(']', i);
      if (e != std::string::npos) { i = e; continue; }
    }
    out += t[i];
  }
  return out;
}

}  // namespace

void test_real_conversations_survive_hidden_mentions() {
  int picks = 0, keptFull = 0, keptMasked = 0, keptLegacyMasked = 0;
  double sizeNew = 0, sizeOld = 0;
  clock_t t0 = clock();
  int calls = 0;
  for (int chan : {0, 13}) {
    Rows rows;
    if (!loadCorpus("test/data/corpus-real.txt", chan, rows) || rows.r.size() < 20) TEST_IGNORE_MESSAGE("no corpus");
    int n = (int)rows.r.size();
    for (int j = 0; j < n; j++) {
      // The message the mention points at: the last one by the person named, before this.
      const std::string& text = rows.r[j].text;
      size_t at0 = text.find("@[");
      size_t e = at0 == std::string::npos ? std::string::npos : text.find(']', at0);
      if (e == std::string::npos) continue;
      std::string name = text.substr(at0 + 2, e - at0 - 2);
      int target = -1;
      for (int i = j - 1; i >= 0 && j - i < THREAD_WINDOW - 8; i--)
        if (rows.r[i].who == name) { target = i; break; }
      if (target < 0) continue;
      picks++;
      int out[THREAD_MAX];
      int got = buildThread(atRows, &rows, n, j, "me", out, THREAD_MAX);
      calls++;
      sizeNew += got;
      keptFull += has(out, got, target);
      int old[THREAD_MAX];
      sizeOld += legacyThread(atRows, &rows, n, j, "me", old, THREAD_MAX);
      // Now with that message's mentions hidden: can the graph still find what it answers?
      std::string saved = rows.r[j].text;
      rows.r[j].text = maskMentions(saved);
      got = buildThread(atRows, &rows, n, j, "me", out, THREAD_MAX);
      calls++;
      keptMasked += has(out, got, target);
      int old2 = legacyThread(atRows, &rows, n, j, "me", old, THREAD_MAX);
      keptLegacyMasked += has(old, old2, target);
      rows.r[j].text = saved;
    }
  }
  double ms = 1000.0 * (double)(clock() - t0) / CLOCKS_PER_SEC / (calls ? calls : 1);
  printf("\nreal conversation shapes: %d messages that mention someone\n  mention found, shown: graph %.0f%%, mentions only %.0f%%\n"
         "  mention hidden:       graph %.0f%%, mentions only %.0f%%\n  average thread: graph %.1f messages, mentions only %.1f; %.2f ms per thread (host)\n",
         picks, 100.0 * keptFull / picks, 100.0 * picks / picks, 100.0 * keptMasked / picks, 100.0 * keptLegacyMasked / picks,
         sizeNew / picks, sizeOld / picks, ms);
  TEST_ASSERT_TRUE(picks >= 50);
  TEST_ASSERT_TRUE(keptFull >= picks * 95 / 100);                // a shown mention is followed
  TEST_ASSERT_TRUE(keptMasked > keptLegacyMasked + picks / 10);   // a hidden one is often found anyway
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_first_mention);
  RUN_TEST(test_no_mention_is_just_the_message);
  RUN_TEST(test_reply_pulls_in_the_last_run_by_that_person);
  RUN_TEST(test_run_stops_at_someone_else);
  RUN_TEST(test_follows_a_chain_of_replies);
  RUN_TEST(test_names_match_without_case_and_outgoing_is_me);
  RUN_TEST(test_unknown_person_and_loops_end);
  RUN_TEST(test_the_limit_is_kept);
  RUN_TEST(test_padded_and_at_names_match);
  RUN_TEST(test_several_mentions_in_one_message_are_all_followed);
  RUN_TEST(test_mentions_in_a_row_are_taken_together);
  RUN_TEST(test_mentions_in_a_row_by_different_people_belong_together);
  RUN_TEST(test_a_message_about_someone_else_stays_out);
  RUN_TEST(test_unmentioned_replies_and_continuations_join);
  RUN_TEST(test_interleaved_conversations_stay_apart);
  RUN_TEST(test_a_mention_resumes_a_conversation_after_a_break);
  RUN_TEST(test_someone_in_two_conversations_does_not_join_them);
  RUN_TEST(test_a_mention_of_someone_not_in_view_and_duplicate_names);
  RUN_TEST(test_the_window_and_its_edges);
  RUN_TEST(test_new_algorithm_recalls_more_than_mentions_alone);
  RUN_TEST(test_real_conversations_survive_hidden_mentions);
  return UNITY_END();
}
