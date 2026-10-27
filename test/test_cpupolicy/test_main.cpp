// The CPU clock rules: Settings picks the speed with the screen on; screen off never goes below 160 MHz.
#include <unity.h>
#include "app/cpu_policy.h"

using app::cpuTargetMhz;
static const app::CpuSpeeds kS3 = {240, 160, 80, 160};

void setUp() {}
void tearDown() {}

void test_active_speeds() {
  TEST_ASSERT_EQUAL(240, cpuTargetMhz(kS3, 0, false));
  TEST_ASSERT_EQUAL(160, cpuTargetMhz(kS3, 1, false));
  TEST_ASSERT_EQUAL(80, cpuTargetMhz(kS3, 2, false));
}

void test_screen_off_is_160_whatever_the_setting() {
  for (uint8_t lvl = 0; lvl < 3; lvl++) TEST_ASSERT_EQUAL(160, cpuTargetMhz(kS3, lvl, true));
}

void test_never_above_the_boards_speed() {
  app::CpuSpeeds s = {80, 160, 80, 160};
  TEST_ASSERT_EQUAL(80, cpuTargetMhz(s, 0, false));
  TEST_ASSERT_EQUAL(80, cpuTargetMhz(s, 1, false));
  TEST_ASSERT_EQUAL(80, cpuTargetMhz(s, 0, true));
}

void test_idle_zero_leaves_the_clock_alone() {
  app::CpuSpeeds s = {240, 160, 80, 0};
  TEST_ASSERT_EQUAL(240, cpuTargetMhz(s, 0, true));
  TEST_ASSERT_EQUAL(80, cpuTargetMhz(s, 2, true));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_active_speeds);
  RUN_TEST(test_screen_off_is_160_whatever_the_setting);
  RUN_TEST(test_never_above_the_boards_speed);
  RUN_TEST(test_idle_zero_leaves_the_clock_alone);
  return UNITY_END();
}
