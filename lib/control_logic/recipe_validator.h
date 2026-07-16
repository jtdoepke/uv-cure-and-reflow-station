// RecipeValidator — the controller's upload-time recipe/start checks (A7).
//
// Plugs into the ISetupValidator seam (lib/protocol/setup_responder.h): the
// SetupResponder calls validateRecipe/validateStart before Acking, so returning
// false with a NakReason turns the upload into a Nak{seq,reason} on the wire and
// applies no side effect (design.md §9). This replaces A2's accept-all default.
//
// The checks (design.md §4 "Mode-dependent temp cap" + §9 Nak reasons):
//   - structural: a recipe must have >=1 segment and every segment a non-zero
//     duration, else NAK_OUT_OF_RANGE.
//   - mode/content mismatch (NAK_MODE_CONTENT_MISMATCH): a recipe *tagged* REFLOW
//     that asserts uv/motor, or one mixing uv with a setpoint above the cure
//     ceiling — the untrusted tag disagreeing with the content is a hard reject.
//   - range (NAK_OUT_OF_RANGE): every setpoint within [floor, hard-max], where the
//     hard-max is chosen from *content* (oven_safety::deriveMode), so uv/motor
//     tighten the whole run to the cure ceiling regardless of the tag.
// validateStart requires the referenced recipe to be the one most recently
// accepted (NAK_UNKNOWN_RECIPE) — a stateless "the controller only starts what it
// just validated" check.
//
// Deferred to their owners (would need ports/state A7 doesn't have): the
// workpiece-TC plausibility check on a reflow Start (NAK_WORKPIECE_TC_INVALID,
// needs the temp-input port — A6/D4) and the already-running guard
// (NAK_ILLEGAL_TRANSITION, needs run state — A6). Runtime measured-temp trips are
// A4b, a separate layer from this upload-time validation.
//
// Header-only, DI-free, no Arduino: unit-testable in native_control.
#pragma once

#include <cmath>
#include <cstdint>

#include "oven.pb.h"
#include "oven_safety.h"
#include "setup_responder.h"

class RecipeValidator : public protocol::ISetupValidator {
public:
  bool validateRecipe(const oven_Recipe &recipe, oven_NakReason &reason) override {
    if (recipe.segments_count == 0) {
      reason = oven_NakReason_NAK_OUT_OF_RANGE;
      return false;
    }

    bool has_uv = false;
    bool has_motor = false;
    float min_heat = recipe.segments[0].heat_c;
    float max_heat = recipe.segments[0].heat_c;
    for (pb_size_t i = 0; i < recipe.segments_count; ++i) {
      const oven_Segment &seg = recipe.segments[i];
      // NaN/Inf off the wire would sail through every magnitude guard below (all
      // comparisons against NaN are false), so reject non-finite setpoints here —
      // this is the untrusted-CYD backstop, it cannot assume well-formed floats.
      if (seg.dur_ms == 0 || !std::isfinite(seg.heat_c)) {
        reason = oven_NakReason_NAK_OUT_OF_RANGE;
        return false;
      }
      has_uv = has_uv || seg.uv;
      has_motor = has_motor || seg.motor;
      min_heat = seg.heat_c < min_heat ? seg.heat_c : min_heat;
      max_heat = seg.heat_c > max_heat ? seg.heat_c : max_heat;
    }

    // Mode/content mismatch: the declared tag must not contradict the content.
    if (recipe.mode == oven_Mode_MODE_REFLOW && (has_uv || has_motor)) {
      reason = oven_NakReason_NAK_MODE_CONTENT_MISMATCH;
      return false;
    }
    if (has_uv && max_heat > oven_safety::CURE_HARD_MAX_C) {
      reason = oven_NakReason_NAK_MODE_CONTENT_MISMATCH;
      return false;
    }

    // Range: setpoints within [floor, content-derived hard-max].
    const float cap = oven_safety::hardMaxForMode(oven_safety::deriveMode(recipe));
    if (max_heat > cap || min_heat < oven_safety::MIN_SEGMENT_C) {
      reason = oven_NakReason_NAK_OUT_OF_RANGE;
      return false;
    }

    last_accepted_id_ = recipe.id;
    have_accepted_ = true;
    return true;
  }

  bool validateStart(const oven_Start &start, oven_NakReason &reason) override {
    // Session 0 is the IDLE sentinel in telemetry (src_control/main.cpp), so a run
    // must never adopt it: an accepted Start{session=0} would read as active on the
    // controller while telemetry still reported IDLE. The CYD already picks non-zero
    // (esp_random() | 1U); the backstop enforces it independently.
    if (start.session == 0) {
      reason = oven_NakReason_NAK_OUT_OF_RANGE;
      return false;
    }
    if (!have_accepted_ || start.recipe_id != last_accepted_id_) {
      reason = oven_NakReason_NAK_UNKNOWN_RECIPE;
      return false;
    }
    return true;
  }

private:
  bool have_accepted_ = false;
  uint32_t last_accepted_id_ = 0;
};
