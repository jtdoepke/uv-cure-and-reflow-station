// oven_safety.h — the controller's OWN reviewed per-mode absolute hard-max
// (design.md §4 layer 1, the "untrusted-CYD-proof backstop").
//
// This is the hand-written, reviewed header the safety model requires to live in
// the *controller* tree — deliberately NOT the CYD's settings_defaults.h (a user
// policy setting the operator can raise) and NOT the generated oven_cal.h (§6). A
// buggy or malicious CYD cannot push a run past these numbers because the
// controller clamps/NAKs against them independently (§9). The CYD-side placeholder
// in settings_defaults.h:22-24 explicitly anticipates this file landing.
//
// The cap selector is derived from recipe *content*, never the declared `mode`
// tag: any segment asserting uv or motor forces the (lower) cure ceiling for the
// whole run, so the untrusted tag can only ever be cross-checked, not trusted to
// pick the cap. Pure constants + inline helpers; no Arduino, host-testable.
#pragma once

#include "oven.pb.h"

namespace oven_safety {

// Absolute per-mode ceilings, in deg C. Reflow = 300 is DECIDED (design.md §4).
constexpr float REFLOW_HARD_MAX_C = 300.0F;
// The UV/cure absolute hard-max is still an OPEN question (§10, "value TBD"). Keep
// it a conservative fixed ceiling matching the CYD-side placeholder
// (settings_defaults::UV_HARD_MAX) until §10 decides; revisit both together.
constexpr float CURE_HARD_MAX_C = 120.0F; // TBD §10

// A setpoint below this is implausible/erroneous, not a real recipe.
constexpr float MIN_SEGMENT_C = 0.0F;

// The cap-selector mode derived from recipe content (NOT recipe.mode). A segment
// asserting uv or motor forces CURE (the tighter cap); a plain-heat recipe with
// neither is treated as REFLOW (the higher cap).
inline oven_Mode deriveMode(const oven_Recipe &recipe) {
  for (pb_size_t i = 0; i < recipe.segments_count; ++i) {
    if (recipe.segments[i].uv || recipe.segments[i].motor) {
      return oven_Mode_MODE_CURE;
    }
  }
  return oven_Mode_MODE_REFLOW;
}

// Absolute hard-max for a cap-selector mode. Anything other than CURE uses the
// reflow ceiling (UNSPECIFIED never selects the tighter cure cap here — content
// derivation, not the tag, is what tightens it).
inline float hardMaxForMode(oven_Mode mode) {
  return mode == oven_Mode_MODE_CURE ? CURE_HARD_MAX_C : REFLOW_HARD_MAX_C;
}

} // namespace oven_safety
