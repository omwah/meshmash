// The battery trend and screen-off time: only rested readings count, charging pauses the trend.
#include <unity.h>
#include "app/power_stats.h"

using app::PowerStats;
static const uint32_t MIN = 60000, HOUR = 3600000;

void setUp() {}
void tearDown() {}

void test_screen_off_time_adds_up() {
  PowerStats s;
  s.reset(0);
  s.screen(true, 10000);
  s.screen(true, 20000);            // already off: nothing changes
  s.screen(false, 70000);
  TEST_ASSERT_EQUAL(60, s.snapshot(70000).screenOffS);
  s.screen(true, 100000);
  TEST_ASSERT_EQUAL(70, s.snapshot(110000).screenOffS);   // the open stretch counts
  TEST_ASSERT_EQUAL(110, s.snapshot(110000).sinceS);
}

void test_reset_keeps_an_open_screen_off_stretch_from_now() {
  PowerStats s;
  s.screen(true, 0);
  s.reset(50000);
  TEST_ASSERT_EQUAL(10, s.snapshot(60000).screenOffS);
  TEST_ASSERT_EQUAL(10, s.snapshot(60000).sinceS);
}

void test_no_reading_while_screen_on_or_unsettled_or_busy() {
  PowerStats s;
  TEST_ASSERT_FALSE(s.sampleDue(HOUR, true));                 // screen on
  s.screen(true, HOUR);
  TEST_ASSERT_FALSE(s.sampleDue(HOUR + 5 * MIN - 1, true));     // still settling
  TEST_ASSERT_FALSE(s.sampleDue(HOUR + 6 * MIN, false));        // transmitting or playing
  TEST_ASSERT_TRUE(s.sampleDue(HOUR + 5 * MIN, true));
}

void test_readings_are_five_minutes_apart() {
  PowerStats s;
  s.screen(true, 0);
  s.sample(4000, false, 40000);
  TEST_ASSERT_FALSE(s.sampleDue(40000 + 5 * MIN - 1, true));
  TEST_ASSERT_TRUE(s.sampleDue(40000 + 5 * MIN, true));
}

void test_rate_needs_an_hour_and_two_readings() {
  PowerStats s;
  s.screen(true, 0);
  s.sample(4000, false, 40000);
  TEST_ASSERT_FALSE(s.snapshot(HOUR).rateKnown);              // one reading
  s.sample(3990, false, 40000 + 30 * MIN);
  TEST_ASSERT_FALSE(s.snapshot(HOUR).rateKnown);              // half an hour
  s.sample(3982, false, 40000 + 2 * HOUR);
  model::Power p = s.snapshot(3 * HOUR);
  TEST_ASSERT_TRUE(p.rateKnown);
  TEST_ASSERT_EQUAL(3982, p.battMv);
  TEST_ASSERT_EQUAL(-9, p.mvPerHour);                         // 18 mV in 2 h
}

void test_usb_host_pauses_the_trend_and_restarts_it_at_the_next_reading() {
  PowerStats s;
  s.screen(true, 0);
  s.sample(4000, false, 40000);
  s.sample(4100, true, 40000 + 5 * MIN);                      // plugged in
  TEST_ASSERT_TRUE(s.snapshot(HOUR).charging);
  TEST_ASSERT_EQUAL(0, s.snapshot(HOUR).battMv);
  s.sample(4150, false, 40000 + 10 * MIN);                    // unplugged but still rising
  TEST_ASSERT_TRUE(s.snapshot(HOUR).charging);
  s.sample(4148, false, 40000 + 15 * MIN);                    // settled: this one is thrown away
  TEST_ASSERT_FALSE(s.snapshot(HOUR).charging);
  TEST_ASSERT_EQUAL(0, s.snapshot(HOUR).battMv);
  s.sample(4140, false, 40000 + 20 * MIN);
  TEST_ASSERT_EQUAL(4140, s.snapshot(HOUR).battMv);
}

void test_a_rise_above_noise_is_charging_without_a_host() {
  PowerStats s;
  s.screen(true, 0);
  s.sample(3900, false, 40000);
  s.sample(3910, false, 40000 + 5 * MIN);                     // within noise
  TEST_ASSERT_FALSE(s.snapshot(HOUR).charging);
  s.sample(3960, false, 40000 + 10 * MIN);
  TEST_ASSERT_TRUE(s.snapshot(HOUR).charging);
}

void test_interval_is_the_latest_screen_off_stretch() {
  PowerStats s;
  s.screen(true, 0);
  s.sample(4000, false, 40000);
  s.sample(3995, false, 40000 + 5 * MIN);
  uint16_t a, b;
  uint32_t span;
  TEST_ASSERT_TRUE(s.interval(a, b, span));
  TEST_ASSERT_EQUAL(4000, a);
  TEST_ASSERT_EQUAL(3995, b);
  TEST_ASSERT_EQUAL(300, span);
  s.screen(false, HOUR);
  s.screen(true, 2 * HOUR);
  TEST_ASSERT_FALSE(s.interval(a, b, span));                  // a new stretch, no reading yet
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_screen_off_time_adds_up);
  RUN_TEST(test_reset_keeps_an_open_screen_off_stretch_from_now);
  RUN_TEST(test_no_reading_while_screen_on_or_unsettled_or_busy);
  RUN_TEST(test_readings_are_five_minutes_apart);
  RUN_TEST(test_rate_needs_an_hour_and_two_readings);
  RUN_TEST(test_usb_host_pauses_the_trend_and_restarts_it_at_the_next_reading);
  RUN_TEST(test_a_rise_above_noise_is_charging_without_a_host);
  RUN_TEST(test_interval_is_the_latest_screen_off_stretch);
  return UNITY_END();
}
