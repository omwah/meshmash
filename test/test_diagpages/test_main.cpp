// Which Diagnostics pages a new and an existing install show.
#include <unity.h>
#include "app/diag_pages.h"

void setUp() {}
void tearDown() {}

void test_new_install_shows_radio_and_system() {
  uint8_t pages = 0, set = 0;
  app::settleDiagPages(pages, set, true);
  TEST_ASSERT_EQUAL(app::DIAG_RADIO | app::DIAG_SYSTEM, pages);
  TEST_ASSERT_EQUAL(1, set);
}

void test_existing_install_keeps_every_page() {
  uint8_t pages = 0, set = 0;   // a file from before: both read as 0
  app::settleDiagPages(pages, set, false);
  TEST_ASSERT_EQUAL(app::DIAG_ALL, pages);
}

void test_a_choice_made_after_the_migration_is_kept() {
  uint8_t pages = app::DIAG_POWER, set = 1;
  app::settleDiagPages(pages, set, false);
  TEST_ASSERT_EQUAL(app::DIAG_POWER, pages);
}

void test_there_is_always_a_page() {
  TEST_ASSERT_EQUAL(app::DIAG_RADIO, app::diagPagesShown(0));
  TEST_ASSERT_EQUAL(app::DIAG_POWER | app::DIAG_STORAGE, app::diagPagesShown(app::DIAG_POWER | app::DIAG_STORAGE));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_new_install_shows_radio_and_system);
  RUN_TEST(test_existing_install_keeps_every_page);
  RUN_TEST(test_a_choice_made_after_the_migration_is_kept);
  RUN_TEST(test_there_is_always_a_page);
  return UNITY_END();
}
