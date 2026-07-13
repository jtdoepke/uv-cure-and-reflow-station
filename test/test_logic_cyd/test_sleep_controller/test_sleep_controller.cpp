// native_logic_cyd suite — pure host tests of the SleepController idle-sleep/wake policy (§17).
// No LVGL/Arduino: time and the `sleepAllowed` predicate are passed in directly.
#include <unity.h>

#include "sleep_controller.h"

void setUp(void) {}
void tearDown(void) {}

void test_sleeps_after_timeout_when_idle(void) {
  SleepController sc; // 2 min default
  sc.tick(0, /*sleepAllowed=*/true);
  TEST_ASSERT_TRUE(sc.awake());
  sc.tick(119999, true); // just under the timeout
  TEST_ASSERT_TRUE(sc.awake());
  sc.tick(120000, true); // timeout reached
  TEST_ASSERT_FALSE(sc.awake());
}

void test_never_sleeps_while_not_allowed(void) {
  SleepController sc;
  sc.tick(0, true);
  // A run / HOT / fault holds sleepAllowed low: never sleep no matter how long, and the timer
  // is held reset so returning to idle grants a full fresh timeout.
  sc.tick(200000, /*sleepAllowed=*/false);
  TEST_ASSERT_TRUE(sc.awake());
  sc.tick(999999, false);
  TEST_ASSERT_TRUE(sc.awake());
  // Back to idle: a full timeout from the last not-allowed tick before it may sleep.
  sc.tick(999999 + 119999, true);
  TEST_ASSERT_TRUE(sc.awake());
  sc.tick(999999 + 120000, true);
  TEST_ASSERT_FALSE(sc.awake());
}

void test_activity_wakes_and_resets_timer(void) {
  SleepController sc;
  sc.tick(0, true);
  sc.tick(120000, true);
  TEST_ASSERT_FALSE(sc.awake()); // asleep

  sc.noteActivity(120000); // a touch / fault / door-open wake
  TEST_ASSERT_TRUE(sc.awake());
  sc.tick(120001, true); // 1 ms since activity -> still awake
  TEST_ASSERT_TRUE(sc.awake());
  sc.tick(240000, true); // another full timeout later -> sleeps again
  TEST_ASSERT_FALSE(sc.awake());
}

void test_timeout_is_configurable(void) {
  SleepController sc;
  sc.setIdleTimeoutMs(1000);
  sc.tick(0, true);
  sc.tick(999, true);
  TEST_ASSERT_TRUE(sc.awake());
  sc.tick(1000, true);
  TEST_ASSERT_FALSE(sc.awake());
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_sleeps_after_timeout_when_idle);
  RUN_TEST(test_never_sleeps_while_not_allowed);
  RUN_TEST(test_activity_wakes_and_resets_timer);
  RUN_TEST(test_timeout_is_configurable);
  return UNITY_END();
}
