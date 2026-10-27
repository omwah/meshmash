// The Cardputer's lock preference: a new install locks, an existing one keeps any-key wake.
#include <unity.h>
#include "app/lock_pref.h"

void setUp() {}
void tearDown() {}

void test_new_install_locks() {
  uint8_t off = 1, set = 0;
  app::settleLockPref(off, set, true, false);
  TEST_ASSERT_EQUAL(0, off);
  TEST_ASSERT_EQUAL(1, set);
}

void test_existing_cardputer_install_keeps_any_key_wake() {
  uint8_t off = 0, set = 0;   // a file from before: the field reads as 0
  app::settleLockPref(off, set, false, false);
  TEST_ASSERT_EQUAL(1, off);
  TEST_ASSERT_EQUAL(1, set);
}

void test_existing_tdeck_install_keeps_its_choice() {
  uint8_t off = 0, set = 0;
  app::settleLockPref(off, set, false, true);
  TEST_ASSERT_EQUAL(0, off);
  off = 1; set = 0;
  app::settleLockPref(off, set, false, true);
  TEST_ASSERT_EQUAL(1, off);
}

void test_a_choice_made_after_the_migration_is_kept() {
  uint8_t off = 0, set = 1;   // the user turned the lock on in Settings
  app::settleLockPref(off, set, false, false);
  TEST_ASSERT_EQUAL(0, off);
  off = 1;
  app::settleLockPref(off, set, false, false);
  TEST_ASSERT_EQUAL(1, off);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_new_install_locks);
  RUN_TEST(test_existing_cardputer_install_keeps_any_key_wake);
  RUN_TEST(test_existing_tdeck_install_keeps_its_choice);
  RUN_TEST(test_a_choice_made_after_the_migration_is_kept);
  return UNITY_END();
}
