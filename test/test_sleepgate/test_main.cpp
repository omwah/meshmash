// When light sleep is allowed, and which condition is reported when it is not.
#include <unity.h>
#include <string.h>
#include "app/sleep_gate.h"

void setUp() {}
void tearDown() {}

static app::SleepInputs quiet() { return {true, true, false, false, false, false, false, false}; }

void test_quiet_locked_screen_off_may_sleep() { TEST_ASSERT_NULL(app::sleepBlock(quiet())); }

void test_each_condition_blocks_with_its_reason() {
  app::SleepInputs s = quiet();
  s.screenOff = false; TEST_ASSERT_EQUAL_STRING("screen", app::sleepBlock(s));
  s = quiet(); s.locked = false; TEST_ASSERT_EQUAL_STRING("unlocked", app::sleepBlock(s));
  s = quiet(); s.bleOn = true; TEST_ASSERT_EQUAL_STRING("BLE on", app::sleepBlock(s));
  s = quiet(); s.usbHost = true; TEST_ASSERT_EQUAL_STRING("USB", app::sleepBlock(s));
  s = quiet(); s.audio = true; TEST_ASSERT_EQUAL_STRING("audio", app::sleepBlock(s));
  s = quiet(); s.gps = true; TEST_ASSERT_EQUAL_STRING("GPS", app::sleepBlock(s));
  s = quiet(); s.pending = true; TEST_ASSERT_EQUAL_STRING("work", app::sleepBlock(s));
  s = quiet(); s.unlockHeld = true; TEST_ASSERT_EQUAL_STRING("button", app::sleepBlock(s));
}

void test_the_first_failing_condition_is_the_one_reported() {
  app::SleepInputs s = quiet();
  s.bleOn = true; s.usbHost = true; s.pending = true;
  TEST_ASSERT_EQUAL_STRING("BLE on", app::sleepBlock(s));
  s.screenOff = false;
  TEST_ASSERT_EQUAL_STRING("screen", app::sleepBlock(s));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_quiet_locked_screen_off_may_sleep);
  RUN_TEST(test_each_condition_blocks_with_its_reason);
  RUN_TEST(test_the_first_failing_condition_is_the_one_reported);
  return UNITY_END();
}
