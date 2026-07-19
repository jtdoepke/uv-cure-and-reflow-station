// native_ui_cyd suite — the press-and-hold arm model (C6b, hold_button.h). Pure timing logic (no
// LVGL calls), so it drives HoldButtonModel with an explicit clock: the ring fills over the dwell,
// completing exactly once, and lifting early cancels with nothing fired. The widget half (arc
// paint, event plumbing) is reviewed via sim-shot.
#include <unity.h>

#include "hold_button.h"

void setUp(void) {}
void tearDown(void) {}

// A full hold fires exactly once, at the dwell edge, and progress ramps 0→1.
void test_hold_completes_once(void) {
  HoldButtonModel m;
  m.configure(1000);
  m.press(0);
  TEST_ASSERT_FALSE(m.poll(0));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, m.progress(0));
  TEST_ASSERT_FALSE(m.poll(500));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.5f, m.progress(500));
  TEST_ASSERT_TRUE(m.poll(1000)); // the arm edge
  TEST_ASSERT_TRUE(m.armed);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.0f, m.progress(1000));
  // Latches: no second fire, progress stays full even past the dwell.
  TEST_ASSERT_FALSE(m.poll(2000));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.0f, m.progress(2000));
}

// Lifting before the dwell cancels: nothing fires and the ring resets to empty.
void test_release_before_dwell_cancels(void) {
  HoldButtonModel m;
  m.configure(1000);
  m.press(0);
  TEST_ASSERT_FALSE(m.poll(600));
  m.release();
  TEST_ASSERT_FALSE(m.armed);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, m.progress(700)); // reset, not lingering at 0.6
  // A later tick past the original dwell must NOT arm (the press was abandoned).
  TEST_ASSERT_FALSE(m.poll(2000));
  TEST_ASSERT_FALSE(m.armed);
}

// Re-pressing after a cancel starts a fresh hold from the new press time.
void test_repress_restarts(void) {
  HoldButtonModel m;
  m.configure(1000);
  m.press(0);
  m.poll(400);
  m.release();
  m.press(5000); // fresh press
  TEST_ASSERT_FALSE(m.poll(5400));
  TEST_ASSERT_TRUE(m.poll(6000));
  TEST_ASSERT_TRUE(m.armed);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_hold_completes_once);
  RUN_TEST(test_release_before_dwell_cancels);
  RUN_TEST(test_repress_restarts);
  return UNITY_END();
}
