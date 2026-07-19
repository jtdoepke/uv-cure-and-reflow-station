#include "run_curve.h"

#include <cmath>

#include "theme.h"

namespace {
int32_t iround(float v) {
  if (!(v == v)) { // NaN → clamp low (the caller already clamps temps, belt-and-braces)
    return 0;
  }
  return static_cast<int32_t>(lroundf(v));
}
} // namespace

RunCurve create_run_curve(lv_obj_t *parent, const float *projected, uint16_t n, int32_t yMinC,
                          int32_t yMaxC) {
  if (n < 2) {
    n = 2;
  }
  if (yMaxC <= yMinC) {
    yMaxC = yMinC + 1; // a degenerate range would collapse the plot; keep it drawable
  }

  lv_obj_t *root = lv_obj_create(parent);
  theme::apply_panel(root); // themed instrument surface
  lv_obj_set_width(root, lv_pct(100));
  lv_obj_set_flex_grow(root, 1);
  lv_obj_set_style_pad_all(root, theme::PAD_S, 0);
  lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *chart = lv_chart_create(root);
  lv_obj_set_size(chart, lv_pct(100), lv_pct(100));
  lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(chart, n);
  lv_chart_set_axis_range(chart, LV_CHART_AXIS_PRIMARY_Y, yMinC, yMaxC);
  lv_chart_set_div_line_count(chart, 3, 0);
  lv_obj_set_style_border_width(chart, 0, 0);
  lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
  lv_obj_set_style_size(chart, 0, 0, LV_PART_INDICATOR); // hide the per-point dots — a clean line
  lv_obj_set_style_line_color(chart, theme::col(theme::ACCENT_DIM), LV_PART_MAIN);
  lv_obj_set_style_line_opa(chart, LV_OPA_30, LV_PART_MAIN);

  // Projected ghost — set once from the pre-sampled trajectory.
  lv_chart_series_t *proj =
      lv_chart_add_series(chart, theme::col(theme::TEXT_DIM), LV_CHART_AXIS_PRIMARY_Y);
  int32_t *proj_y = lv_chart_get_series_y_array(chart, proj);
  for (uint16_t i = 0; i < n; ++i) {
    proj_y[i] = iround(projected[i]);
  }

  // Actual — empty until telemetry fills it.
  lv_chart_series_t *act =
      lv_chart_add_series(chart, theme::col(theme::ACCENT), LV_CHART_AXIS_PRIMARY_Y);
  lv_chart_set_all_values(chart, act, LV_CHART_POINT_NONE);

  lv_chart_cursor_t *now = lv_chart_add_cursor(chart, theme::col(theme::ACCENT), LV_DIR_VER);
  lv_chart_refresh(chart);

  RunCurve rc;
  rc.root = root;
  rc.chart = chart;
  rc.projected = proj;
  rc.actual = act;
  rc.now = now;
  rc.actual_y = lv_chart_get_series_y_array(chart, act);
  rc.points = n;
  rc.last_idx = -1;
  rc.deviating = false;
  return rc;
}

void run_curve_push_actual(RunCurve &rc, float frac01, float valueC, bool deviating) {
  if (rc.chart == nullptr || rc.actual_y == nullptr || rc.points < 2) {
    return;
  }
  if (frac01 < 0.0f) {
    frac01 = 0.0f;
  } else if (frac01 > 1.0f) {
    frac01 = 1.0f;
  }
  int32_t idx = iround(frac01 * static_cast<float>(rc.points - 1));
  if (idx < 0) {
    idx = 0;
  } else if (idx >= static_cast<int32_t>(rc.points)) {
    idx = rc.points - 1;
  }
  const int32_t v = iround(valueC);

  // Fill from the last filled index up to here so the polyline stays continuous even if the run
  // advanced several indices between frames; a same-index push just refreshes the latest value.
  const int32_t from = (idx > rc.last_idx) ? rc.last_idx + 1 : idx;
  for (int32_t j = from; j <= idx; ++j) {
    rc.actual_y[j] = v;
  }
  if (idx > rc.last_idx) {
    rc.last_idx = idx;
  }

  if (deviating != rc.deviating) {
    lv_chart_set_series_color(rc.chart, rc.actual,
                              theme::col(deviating ? theme::WARN : theme::ACCENT));
    rc.deviating = deviating;
  }
  lv_chart_set_cursor_point(rc.chart, rc.now, rc.actual, static_cast<uint32_t>(idx));
  lv_chart_refresh(rc.chart);
}
