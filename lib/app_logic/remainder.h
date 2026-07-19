// remainder.h — the cure resume generator (design.md §15 "Door-open during a run", backlog B6).
//
// §15 DECIDED that a door-open ends the run on the controller, which stays a stateless profile
// executor: "no pause state, no resume logic, no context retained". All the resume intelligence is
// the CYD's, and this is it — the pure part. Given the profile that was running and how far it got,
// build the profile that finishes the job:
//
//   an RAMP_ASAP re-heat to the current target + the remaining hold/phases + remaining UV dose
//
// which the CYD then Starts as an ordinary fresh run. To the controller it is just a new profile —
// that is the whole point, and why a controller reset mid-pause loses nothing.
//
// TWO THINGS THIS DELIBERATELY DOES NOT DO:
//
//  - It does not model the cool-down. §15: "the board's cool-down is handled for free by the ASAP
//    re-heat ramp at the front of the remainder" — the chamber has dropped while the door was open,
//    and an ASAP ramp simply takes as long as it takes to climb back. Nothing here needs to know
//    the current temperature, which is also what keeps it a pure function of the authored profile.
//  - It does not resume REFLOW. §15 is explicit: "reflow → aborted; no resume (reflow can't survive
//    the thermal excursion — decided)". Asked to build one anyway it returns false, rather than
//    quietly producing a profile the design says must not exist.
//
// Pure C++ over phase.h/profile_draft.h — no LVGL, no protobuf, no clock. Host-tested under
// native_logic_cyd.
#pragma once

#include <cstddef>

#include "phase.h"
#include "profile_draft.h"

// NAMESPACE NAME: `cure_resume`, not the `remainder` §15 calls it. `remainder()` is a C99 math
// function in the GLOBAL namespace (<math.h>), which every translation unit here transitively has,
// so `remainder::build(...)` does not parse — the compiler resolves `remainder` to the function.
namespace cure_resume {

// Below this much of a phase's hold still outstanding, the phase counts as finished and the
// remainder starts at the NEXT one. Without a floor, a door opened in the last second of a soak
// would generate a re-heat to target for a hold of ~0 s — all of the thermal cost of a phase for
// none of its effect. 2% is a placeholder like the rest of the §10 thresholds.
inline constexpr float kPhaseDoneFrac = 0.02f;

// Build the resume profile for `original` interrupted during authored phase `phaseIndex`, with
// `holdDelivered01` of that phase's hold/dose already delivered (clamped into [0,1]; a non-finite
// value is treated as 0, i.e. redo the phase — the conservative direction for a cure, since an
// under-exposed part is a scrapped part and an over-exposed one usually is not).
//
// Returns false when there is nothing left to run: a reflow profile, an out-of-range phase (the
// run was in its cool tail, or had not entered a phase yet), or a last phase already complete.
// `out` is only written on success.
inline bool build(const ProfileDraft &original, size_t phaseIndex, float holdDelivered01,
                  ProfileDraft &out) {
  if (original.mode != RecipeMode::Cure) {
    return false; // §15: reflow aborts, it does not resume
  }
  if (phaseIndex >= original.phaseCount || original.phaseCount == 0) {
    return false; // no authored phase was in progress (cool tail, or never started)
  }

  // Clamp/finite-guard the delivered fraction. `!(x >= 0)` catches NaN as well as negatives.
  float delivered = holdDelivered01;
  if (!(delivered >= 0.0f)) {
    delivered = 0.0f;
  } else if (delivered > 1.0f) {
    delivered = 1.0f;
  }
  const float remainingFrac = 1.0f - delivered;

  // A hold that is effectively finished carries the run to the next phase instead of re-heating for
  // a sliver of soak.
  size_t first = phaseIndex;
  float firstFrac = remainingFrac;
  if (remainingFrac <= kPhaseDoneFrac) {
    first = phaseIndex + 1;
    firstFrac = 1.0f;
    if (first >= original.phaseCount) {
      return false; // the last phase finished — the run was over bar the cool tail
    }
  }

  out = ProfileDraft{};
  // Same name and mode: the operator is finishing the SAME job, and the Run screen's header reads
  // this. A "(resume)" suffix was tempting and rejected — kProfileNameCap is tight enough that it
  // would truncate real names, and the screen already says what happened.
  for (size_t i = 0; i < kProfileNameCap; ++i) {
    out.name[i] = original.name[i];
  }
  out.name[kProfileNameCap - 1] = '\0';
  out.mode = original.mode;
  out.stock = false; // a generated one-off, never a library entry

  size_t n = 0;
  for (size_t src = first; src < original.phaseCount && n < kMaxPhases; ++src, ++n) {
    out.phases[n] = original.phases[src];
    if (src == first) {
      // §15's "RAMP_ASAP re-heat to the current target": x = 0 means as-fast-as-possible, which is
      // exactly the recovery from however far the chamber fell while the door was open. The
      // authored ramp time is meaningless now — it described a climb from the PREVIOUS phase.
      out.phases[n].rampSeconds = 0.0f;
      // Scale BOTH hold authorings by what is left. Cure holds are authored as UV dose and compile
      // to seconds via beamCoverage, but fall back to raw holdSeconds when the turntable is off or
      // the model is uncalibrated (§5/B1) — scaling only the dose would silently redo the full soak
      // on exactly the pre-calibration path this project is on today.
      out.phases[n].exposurePerSurface *= firstFrac;
      out.phases[n].holdSeconds *= firstFrac;
    }
  }
  out.phaseCount = n;
  return n > 0;
}

} // namespace cure_resume
