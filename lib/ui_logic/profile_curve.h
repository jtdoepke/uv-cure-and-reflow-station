// profile_curve — a read-only temperature-vs-time curve widget (design.md §12, §23; backlog C4/C5).
//
// Draws a stored/edited profile's trajectory as polylines in a themed instrument box: the
// *requested* curve (what was authored) as a dim/amber dashed ghost, the *achievable* curve
// (requested rate-limited by the calibrated envelope) solid in the accent, and an optional faint
// *overshoot* (closed-loop settling) trace. It also annotates the chart: dashed **phase-separator**
// verticals with time ticks below the plot, **phase names** along the inside bottom, min/max
// **temperature** ticks on the Y axis, and a shaded band over the phases whose **UV** lamp is on
// (cure). All point/annotation math lives in profile_facts.h (host-tested + fuzzed); this file only
// scales already-bounded values to pixels and styles them.
//
// The input arrays are copied into LVGL-owned buffers freed when the widget is deleted, so the
// caller's arrays need not outlive the call (create-on-demand friendly, no PSRAM).
//
// LVGL-only; compiles for firmware and the native_ui_cyd / native_sim host targets.
#pragma once

#include <lvgl.h>

#include "profile_facts.h"

// Everything the curve draws, as one options struct (the widget grew past a sensible positional
// arg-list). All series are optional — a null pointer / zero count omits that layer. `phase_names`
// is parallel to `boundaries` (one per phase); a name may be null to skip that label.
struct ProfileCurveData {
  const profile_facts::CurvePoint *requested = nullptr;
  size_t n_requested = 0;
  const profile_facts::CurvePoint *overshoot = nullptr; // the settling trace (predicted actual, C5)
  size_t n_overshoot = 0;
  const float *boundaries = nullptr; // phase-end times → separator verticals + time ticks
  size_t n_boundaries = 0;
  const profile_facts::TimeSpan *uv_spans = nullptr; // UV-on windows → shaded bands (cure)
  size_t n_uv_spans = 0;
  const char *const *phase_names = nullptr; // one per phase, drawn along the inside bottom
  size_t n_phase_names = 0;
  // Start time (s) of the implicit passive cool-down phase (implicit_cool.h, §6). A hot reflow's
  // slow coast to touch-safe dwarfs the authored phases on a linear time axis, crushing them (and
  // their labels) into the left edge; giving this lets the widget compress that tail to a bounded
  // pixel share so the authored profile stays legible. <0 ⇒ no cool tail ⇒ plain linear axis.
  float cool_start = -1.0f;
  bool uncalibrated = false; // adds the §12 "preview is idealized" note (omit if a banner says so)
  bool rate_limited = false; // draws the requested line amber (the §12 divergence flag)
};

// Handles into the built widget so callers/tests can inspect it.
struct ProfileCurve {
  lv_obj_t *root;   // themed panel
  lv_obj_t *plot;   // the coordinate box the traces + phase labels are drawn in
  lv_obj_t *legend; // the line/band legend below the plot
  lv_obj_t *requested_line;
  lv_obj_t *overshoot_line; // the settling trace, or nullptr
  lv_obj_t *note;           // "uncalibrated" caption, or nullptr
  size_t separators;        // phase-separator verticals drawn
  size_t uv_bands;          // UV shading bands drawn
};

// Build the curve under `parent` from `d`. Requires ui_subjects_init()/lv_init() as usual.
ProfileCurve create_profile_curve(lv_obj_t *parent, const ProfileCurveData &d);
