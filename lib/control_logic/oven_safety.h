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

#include <cstdint>

#include "oven.pb.h"
#include "touch_safe.h" // the ONE codebase-wide touch-safe temperature (oven_domain::kTouchSafeC)

namespace oven_safety {

// Absolute per-mode ceilings, in deg C. Reflow = 300 is DECIDED (design.md §4).
constexpr float REFLOW_HARD_MAX_C = 300.0F;
// The UV/cure absolute hard-max is still an OPEN question (§10, "value TBD"). Keep
// it a conservative fixed ceiling matching the CYD-side placeholder
// (settings_defaults::UV_HARD_MAX) until §10 decides; revisit both together.
constexpr float CURE_HARD_MAX_C = 120.0F; // TBD §10

// A setpoint below this is implausible/erroneous, not a real recipe.
constexpr float MIN_SEGMENT_C = 0.0F;

// The touch-safe chamber temperature a run must cool below before it reports DONE (design.md
// §5/§6). The CYD's recipe compiler appends a passive cool-down segment aiming for this, but the
// controller ALSO enforces its own independent backup cooldown to this threshold on MEASURED
// temperature: it will not leave the run until the control sensor confirms touch-safe, so an
// optimistic or absent compiled cool tail cannot hand the operator a still-hot chamber. The value
// is the single shared oven_domain::kTouchSafeC (touch_safe.h) — a reviewed compile-time constant
// baked in here, never wire data trusted from the CYD (touch_safe.h explains why sharing the
// source is safe). The CYD's implicit_cool.h reads the same source, so the two cannot drift.
constexpr float TOUCH_SAFE_C = oven_domain::kTouchSafeC;

// --- L3 clamp thresholds (design.md §4 "L3 clamps", backlog A4b) ------------------
// These bound the *independent* safety layer that acts on MEASURED temperature, so it
// catches a welded SSR even when the control loop or executor misbehave. Every value
// here is a TBD §10 placeholder ("over-temp-trip / stuck-heater margins + times", §8
// step 4), deliberately conservative and tuned against real runs later — the same
// convention as ProfileExecutor::Config's watchdog constants. They are NOT calibration
// outputs: they live in this reviewed controller header, never the generated oven_cal.h.

// The per-mode over-temp trip fires when a measured high-limit reading exceeds
// hardMaxForMode(mode) + this margin, opening the contactor (Fault{OVERTEMP_CHAMBER}).
// Above the hard-max cap but below the L0 hardware high-limit — firmware catches it first.
constexpr float OVERTEMP_MARGIN_C = 15.0F; // TBD §10

// Stuck-heater plausibility (Fault{HEATER_STUCK}): measured temp climbing while the
// commanded duty is ~0 means the SSR is welded on. Trip when the measured high-limit
// rises by at least STUCK_HEATER_RISE_C across a STUCK_HEATER_WINDOW_MS window during
// which the commanded duty never exceeds STUCK_HEATER_DUTY_EPS.
constexpr float STUCK_HEATER_DUTY_EPS = 0.02F;      // TBD §10: "duty ~ 0" threshold
constexpr float STUCK_HEATER_RISE_C = 5.0F;         // TBD §10: implausible rise over the window
constexpr uint32_t STUCK_HEATER_WINDOW_MS = 60000U; // TBD §10: the N-second window

// Bounded total runtime (Fault{RUNTIME_EXCEEDED}): at run start the supervisor budgets
// Σ(projected segment durations) × this fraction. B1 baked each segment's projected
// duration into dur_ms, so the sum needs no oven_cal here. A run outliving its budget
// faults — the backstop against a run that never reaches DONE.
constexpr float RUNTIME_MARGIN_FRAC = 1.5F; // TBD §10: L3 total-runtime margin

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
