// native_ui_cyd suite — the §15/C7b projected-vs-actual chart widget (run_curve.h). Drives the
// growing-actual push logic: a pushed point lands at the index for its time fraction, the line is
// filled continuously across a fast advance, out-of-range fractions clamp, and the deviation flag
// is accepted without disturbing the values. The visual (ghost + trace + now-marker + amber) is
// reviewed via `make sim-shot`.
#include <unity.h>
#include <lvgl.h>
#include "src/debugging/test/lv_test.h"

#include "panel.h"
#include "run_curve.h"
#include "subjects.h"

void setUp(void) {
  lv_init();
  lv_test_display_create(panel::W, panel::H);
  ui_subjects_init();
}
void tearDown(void) {
  lv_deinit();
}

// A pushed sample lands at the index for its time fraction; earlier gaps fill so the line is
// continuous; a later push extends it.
void test_push_fills_continuously(void) {
  const float proj[8] = {25, 60, 120, 180, 220, 245, 120, 45};
  RunCurve rc = create_run_curve(lv_screen_active(), proj, 8, 0, 300);
  TEST_ASSERT_NOT_NULL(rc.chart);
  TEST_ASSERT_EQUAL_INT(-1, rc.last_idx);

  run_curve_push_actual(rc, 0.0f, 26.0f, false); // idx 0
  TEST_ASSERT_EQUAL_INT(0, rc.last_idx);
  TEST_ASSERT_EQUAL_INT32(26, rc.actual_y[0]);

  // A jump to the middle fills the skipped indices so the polyline has no gap.
  run_curve_push_actual(rc, 4.0f / 7.0f, 200.0f, false); // idx 4
  TEST_ASSERT_EQUAL_INT(4, rc.last_idx);
  TEST_ASSERT_EQUAL_INT32(200, rc.actual_y[1]);
  TEST_ASSERT_EQUAL_INT32(200, rc.actual_y[4]);
  // Points past the leading edge are still unset (LV_CHART_POINT_NONE).
  TEST_ASSERT_EQUAL_INT32(LV_CHART_POINT_NONE, rc.actual_y[5]);

  run_curve_push_actual(rc, 1.0f, 44.0f, true); // idx 7, deviating
  TEST_ASSERT_EQUAL_INT(7, rc.last_idx);
  TEST_ASSERT_EQUAL_INT32(44, rc.actual_y[7]);
  TEST_ASSERT_TRUE(rc.deviating);
}

// A frac past [0,1] clamps to the end index rather than indexing out of the buffer.
void test_frac_clamps(void) {
  const float proj[4] = {25, 100, 200, 40};
  RunCurve rc = create_run_curve(lv_screen_active(), proj, 4, 0, 300);
  run_curve_push_actual(rc, 5.0f, 199.0f, false); // frac > 1 → last index (3)
  TEST_ASSERT_EQUAL_INT(3, rc.last_idx);
  TEST_ASSERT_EQUAL_INT32(199, rc.actual_y[3]);
  run_curve_push_actual(rc, -1.0f, 10.0f,
                        false); // frac < 0 → index 0, but already past → refresh 0
  TEST_ASSERT_EQUAL_INT32(10, rc.actual_y[0]);
}

// A same-index re-push (frames faster than the chart resolution) just refreshes the latest value.
void test_same_index_refresh(void) {
  const float proj[4] = {25, 100, 200, 40};
  RunCurve rc = create_run_curve(lv_screen_active(), proj, 4, 0, 300);
  run_curve_push_actual(rc, 0.34f, 90.0f, false); // idx 1
  run_curve_push_actual(rc, 0.34f, 95.0f, false); // same idx 1 → overwrite
  TEST_ASSERT_EQUAL_INT(1, rc.last_idx);
  TEST_ASSERT_EQUAL_INT32(95, rc.actual_y[1]);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_push_fills_continuously);
  RUN_TEST(test_frac_clamps);
  RUN_TEST(test_same_index_refresh);
  return UNITY_END();
}
