// Threads found by following mentions back.
#include <unity.h>
#include <stdio.h>
#include <string.h>
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
  // A message in the run that mentions no one ends it.
  Chat d;
  d.add("Alice", "anyone?");                 // 0
  d.add("Carol", "@[Alice] hi");             // 1
  d.add("Carol", "just talking");            // 2
  d.add("Carol", "@[Alice] again");          // 3
  TEST_ASSERT_EQUAL(2, thread(d, 3, t));
  TEST_ASSERT_EQUAL(0, t[0]);
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

void test_the_run_stops_at_a_message_about_someone_else() {
  Chat c;
  c.add("Egg", "hello");                     // 0
  c.add("Alice", "@[Egg] hi");               // 1
  c.add("Bob", "@[Zed] unrelated mention");  // 2
  c.add("Carol", "@[Egg] hey");              // 3
  int t[16];
  TEST_ASSERT_EQUAL(2, thread(c, 1, t));     // 0 and 1: Bob's message ends the run
  TEST_ASSERT_EQUAL(2, thread(c, 3, t));     // the egg and Carol; Bob's is in the way
  TEST_ASSERT_EQUAL(0, t[0]);
  TEST_ASSERT_EQUAL(3, t[1]);
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
  RUN_TEST(test_the_run_stops_at_a_message_about_someone_else);
  return UNITY_END();
}
