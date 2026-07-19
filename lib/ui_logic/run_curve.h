// run_curve — the §15 projected-vs-actual live chart (backlog C7b). The visual payoff of the Run
// screen: the projected achievable trajectory (a dim ghost, computed once from the recipe) with the
// measured control temperature drawn OVER it as the run progresses, plus a now-marker at the
// leading edge. When the run drifts off plan (RunTracker::deviating / §16's DeviationMonitor), the
// actual trace turns amber — the live cue that something is wrong, the same signal the Done summary
// reads.
//
// Built on lv_chart (fixed point count over the run's [0, total] time span): the projected series
// is set once; the actual series is grown one point at a time as telemetry frames arrive, so
// redraws are cheap (no full rebuild per frame). All temperature math is the caller's (RunTracker);
// this widget only scales already-bounded values to the chart.
//
// LVGL-only; compiles for firmware and the native_ui_cyd / native_sim host targets.
#pragma once

#include <cstdint>

#include <lvgl.h>

// Handles into the built chart. The caller pushes actual points via run_curve_push_actual and must
// not use a RunCurve whose widget tree has been deleted (chart == nullptr guards a stale push).
struct RunCurve {
  lv_obj_t *root = nullptr;  // themed plot panel
  lv_obj_t *chart = nullptr; // the lv_chart
  lv_chart_series_t *projected = nullptr;
  lv_chart_series_t *actual = nullptr;
  lv_chart_cursor_t *now = nullptr;
  int32_t *actual_y =
      nullptr;            // the actual series' value buffer (stable while point_count is fixed)
  uint16_t points = 0;    // N — chart resolution across the run
  int32_t last_idx = -1;  // highest actual index filled so far (-1 = none)
  bool deviating = false; // last-applied cue colour
};

// Build the run chart under `parent`. `projected` holds `n` evenly-time-spaced projected
// temperatures (°C) spanning the run [0, total]; the y-axis spans [yMinC, yMaxC]. Grows to fill its
// flex slot.
RunCurve create_run_curve(lv_obj_t *parent, const float *projected, uint16_t n, int32_t yMinC,
                          int32_t yMaxC);

// Append the measured control temperature `valueC` at time fraction `frac01` (0..1 of the run). The
// actual line grows continuously up to that point (any indices skipped by a fast advance are
// filled), the now-marker moves there, and the trace turns amber while `deviating`.
void run_curve_push_actual(RunCurve &rc, float frac01, float valueC, bool deviating);

// Replay a retained per-index sample buffer onto a freshly built chart: `actual[0..lastIdx]` are
// the measured temperatures already binned to chart indices. Used by the §16 summary, which
// redraws the COMPLETED run after the live page's widget tree (and its value buffer) is gone —
// and by a mid-run rebuild, so a re-shown Running page comes back with its history intact.
// `lastIdx < 0` leaves the series empty.
void run_curve_set_actual(RunCurve &rc, const float *actual, int32_t lastIdx, bool deviating);

// Punch a HOLE in the measured trace at `idx`, breaking the polyline there. Used by a resumed run
// (§15): nothing was measured while the door stood open, and a line joining the two legs would
// assert a temperature history that was never sampled. A gap says "there is no data here", which
// is the truth.
void run_curve_set_gap(RunCurve &rc, int32_t idx);
