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

// The chamber heats up while the screen is already asleep: sleepAllowed drops from true to false
// (the run-state subject crosses into HOT as the temp passes touch-safe), and the very next tick()
// must relight the screen — a hot chamber behind a dark screen is exactly the §17/§22 hazard the
// policy exists to prevent. This is the SleepController half of "wake up if the temp rises above
// touch-safe while asleep"; the firmware also drives it immediately via the on_run_state observer.
void test_wakes_when_sleep_disallowed_while_asleep(void) {
  SleepController sc;
  sc.tick(0, /*sleepAllowed=*/true);
  sc.tick(120000, true);
  TEST_ASSERT_FALSE(sc.awake()); // asleep, machine was idle and cool

  // Chamber climbs past touch-safe -> sleepAllowed goes false -> the screen wakes.
  sc.tick(120001, /*sleepAllowed=*/false);
  TEST_ASSERT_TRUE(sc.awake());
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

// Regression (found on the 3.5" panel): the waking touch is recorded from inside
// lv_timer_handler(), which main.cpp's loop runs AFTER it sampled `now` — so noteActivity()
// routinely carries a timestamp a few ms LATER than the nowMs of the tick() that follows it.
// Subtracting those unsigned wrapped to ~4.29e9, cleared any timeout instantly, and put the
// screen back to sleep on the very next tick after every wake. The 2.8" board hid this for as
// long as it did because its flush usually returns inside the same millisecond; the 3.5" has
// twice the pixels and crosses a millisecond boundary nearly every iteration.
void test_activity_timestamped_after_now_stays_awake(void) {
  SleepController sc;
  sc.tick(0, true);
  sc.noteActivity(17050); // stamped inside the LVGL callback...
  sc.tick(17000, true);   // ...while the loop's `now` predates it
  TEST_ASSERT_TRUE(sc.awake());
  sc.tick(17100, true);
  TEST_ASSERT_TRUE(sc.awake());
  // And the timeout still runs from the activity, not from the skewed tick.
  sc.tick(17050 + 119999, true);
  TEST_ASSERT_TRUE(sc.awake());
  sc.tick(17050 + 120000, true);
  TEST_ASSERT_FALSE(sc.awake());
}

// The skew guard must not cost us wrap-safety: millis() rolls over every ~49 days, and the oven
// is a machine that can sit powered for months.
void test_survives_millis_wrap(void) {
  SleepController sc;
  const uint32_t near_wrap = 0xFFFFFF00u;
  sc.tick(near_wrap, true);
  sc.noteActivity(near_wrap);
  sc.tick(near_wrap + 119999u, true); // wraps through zero
  TEST_ASSERT_TRUE(sc.awake());
  sc.tick(near_wrap + 120000u, true);
  TEST_ASSERT_FALSE(sc.awake());
}

// A wake tap lights the screen and then the UI must ignore touches briefly, so the second tap of
// a reach-and-double-tap does not actuate whatever the UI just drew under the finger (§17).
void test_wake_guards_input_for_a_beat(void) {
  SleepController s(SleepController::Config{1000, 1000}); // 1 s idle, 1 s guard
  s.tick(0, true);
  s.tick(2000, true);
  TEST_ASSERT_FALSE(s.awake()); // slept

  s.noteActivity(2000); // the wake tap
  TEST_ASSERT_TRUE(s.awake());
  TEST_ASSERT_TRUE(s.inputGuarded(2000));  // the tap that woke it
  TEST_ASSERT_TRUE(s.inputGuarded(2500));  // and a second one mid-guard
  TEST_ASSERT_TRUE(s.inputGuarded(2999));  // right up to the edge
  TEST_ASSERT_FALSE(s.inputGuarded(3000)); // then input works again

  // Touching while already awake must NOT re-arm the guard, or every tap would be swallowed.
  s.noteActivity(3100);
  TEST_ASSERT_FALSE(s.inputGuarded(3100));
}

// Nothing woke, so nothing is guarded: a freshly booted screen is usable immediately.
void test_boot_is_not_guarded(void) {
  SleepController s;
  s.tick(0, true);
  TEST_ASSERT_TRUE(s.awake());
  TEST_ASSERT_FALSE(s.inputGuarded(0));
  TEST_ASSERT_FALSE(s.inputGuarded(500));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_wake_guards_input_for_a_beat);
  RUN_TEST(test_boot_is_not_guarded);
  RUN_TEST(test_sleeps_after_timeout_when_idle);
  RUN_TEST(test_never_sleeps_while_not_allowed);
  RUN_TEST(test_activity_wakes_and_resets_timer);
  RUN_TEST(test_wakes_when_sleep_disallowed_while_asleep);
  RUN_TEST(test_timeout_is_configurable);
  RUN_TEST(test_activity_timestamped_after_now_stays_awake);
  RUN_TEST(test_survives_millis_wrap);
  return UNITY_END();
}
