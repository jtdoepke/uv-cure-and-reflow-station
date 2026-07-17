// profile_curve — a read-only temperature-vs-time curve widget (design.md §12, §23; backlog C4).
//
// Draws a stored profile's trajectory as two polylines in a themed instrument box: the *requested*
// curve (what was authored) as a dim dashed ghost, and the *achievable* curve (requested
// rate-limited by the calibrated envelope) solid in the accent. This is the minimal first cut of
// the §12 feasibility preview; C5 extends the SAME widget to amber divergence flags and the
// closed-loop overshoot. The point maths lives in profile_facts.h (host-tested) so C5 reuses it —
// this file only scales already-bounded points to pixels and styles the lines.
//
// The point arrays are copied into LVGL-owned buffers freed when the lines are deleted, so the
// caller's CurvePoint arrays need not outlive this call (create-on-demand friendly, no PSRAM).
//
// LVGL-only; compiles for firmware and the native_ui_cyd / native_sim host targets.
#pragma once

#include <lvgl.h>

#include "profile_facts.h"

// Handles into the built widget so callers/tests can inspect it.
struct ProfileCurve {
  lv_obj_t *root; // themed panel
  lv_obj_t *plot; // the coordinate box the lines are drawn in
  lv_obj_t *requested_line;
  lv_obj_t *achievable_line;
  lv_obj_t *note; // "uncalibrated — idealized" caption, or nullptr
};

// Build the curve under `parent`. `req`/`ach` are the requested/achievable point series from
// profile_facts::sampleCurve() (both share one set of axes); either may be empty. `uncalibrated`
// adds the §12 "preview is idealized" note. Requires ui_subjects_init()/lv_init() as usual.
ProfileCurve create_profile_curve(lv_obj_t *parent, const profile_facts::CurvePoint *req,
                                  size_t n_req, const profile_facts::CurvePoint *ach, size_t n_ach,
                                  bool uncalibrated);
