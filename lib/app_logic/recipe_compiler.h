// recipe_compiler.h — lower the CYD's authored Phase[] into the wire oven_Recipe (design.md §5,
// §12; backlog B1). Folds in the fan-Auto resolver (B3, fan_resolver.h).
//
// The operator edits phases (target / ramp x / hold y + channels, phase.h); the controller executes
// generic segments. This compiler is the bridge and the sole place the domain form becomes wire
// form:
//   - each phase -> a ramp segment (RAMP_OVER_TIME when x>0, RAMP_ASAP when x=0) + a hold segment,
//     degenerate ones omitted to stay within the 32-segment wire budget (oven.options);
//   - a cure phase's hold is computed from its authored UV-exposure-per-surface via beamCoverage,
//     falling back to raw seconds when the turntable is off or there is no coverage data (§5);
//   - fan `Auto` is resolved to on/off here (no Auto crosses the wire, §9).
//
// Two-tier validation (§12): a *hard* tier that rejects anything the controller's RecipeValidator
// (A7) would NAK, so an accepted compile always uploads cleanly; and an *amber* tier that only
// warns (rate-limited ramps, uncalibrated/estimated holds, heuristic fans) — those stay saveable
// and uploadable, they just take their real time on the controller.
//
// The oven model, the caps, and the ambient start temperature are all passed by argument (never
// reached into): the compiler holds no policy of its own and stays toy-testable, matching
// thermal_math.h. Pure C++, no LVGL/Arduino — host-tested under native_logic_cyd.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "fan_resolver.h"
#include "implicit_cool.h"
#include "oven.pb.h"
#include "phase.h"
#include "thermal_math.h"

// The wire budget: nanopb statically allocates Recipe.segments at 32 (oven.options). A compile that
// would emit more is hard-rejected.
inline constexpr size_t kMaxSegments = 32;

// Advisory slots. Each meaningful phase emits >=1 segment, so no uploadable recipe carries more
// than kMaxSegments phases; the array is sized to match and writes past it are simply dropped (only
// a pathological run of temperature-and-hold-free phases could get there, and it emits nothing).
// kMaxPhases itself lives in phase.h (the domain fact ProfileStore also sizes to); pin the
// invariant.
static_assert(kMaxPhases == kMaxSegments, "advisory slots must match the segment budget");

// Absolute per-mode temperature bounds the compile is validated against. Passed in from the call
// site (mode + SettingsStore user caps / settings_defaults hard-max); the compiler never picks
// them.
struct Caps {
  float minC; // a setpoint below this is not a real recipe (settings mirror of MIN_SEGMENT_C)
  float capC; // the mode's ceiling (the user cap, itself clamped to the hard-max at boot)
};

// Why a compile was hard-rejected (recipe not uploadable). None => hardValid.
enum class CompileReject : uint8_t {
  None = 0,
  NoPhases,            // empty phase list
  NonFiniteTarget,     // a target temp was NaN/Inf
  TargetOutOfRange,    // a target fell outside [caps.minC, caps.capC]
  ModeContentMismatch, // a Reflow recipe asserted uv/motor
  TooManySegments,     // the phases compile to more than 32 segments
};

// Per-phase amber advisories (all non-fatal; the recipe still uploads).
struct PhaseAdvisory {
  bool rampRateLimited = false; // requested ramp faster than the calibrated envelope allows (§12)
  bool holdEstimated = false;   // cure hold computed from a not-yet-calibrated beamCoverage
  bool holdFallback = false; // a cure phase wanted dose timing but used raw seconds (no coverage)
  bool fanHeuristic = false; // a fan Auto was resolved by the pre-calibration heuristic
};

// The authored phase a compiled segment came from, indexed by segment position. The implicit
// cool-down tail belongs to no authored phase and carries this sentinel — the Run/Monitor tracker
// (C7) maps telemetry seg_idx back through this to name the current phase and drive the per-phase
// run-fit checks (each phase compiles to 0-2 segments, so seg_idx != phase index).
inline constexpr uint8_t kCoolSegment = 0xFF;

struct CompileResult {
  oven_Recipe recipe = oven_Recipe_init_default; // filled iff hardValid
  bool hardValid = true;
  CompileReject reject = CompileReject::None;
  size_t rejectPhase = 0;           // index of the offending phase when !hardValid
  bool uncalibratedPreview = false; // whole preview is idealized-linear (!model.calibrated, §12)
  PhaseAdvisory phases[kMaxPhases] = {};
  size_t phaseCount = 0;
  // Per-segment authored-phase index (kCoolSegment for the implicit cool tail); segmentPhase[j]
  // pairs with recipe.segments[j]. Only the first recipe.segments_count entries are meaningful.
  uint8_t segmentPhase[kMaxSegments] = {};

  // Any reason the UI should show amber (per-phase flag or the whole-recipe uncalibrated preview).
  bool hasAmber() const {
    if (uncalibratedPreview)
      return true;
    for (size_t i = 0; i < phaseCount; ++i) {
      const PhaseAdvisory &a = phases[i];
      if (a.rampRateLimited || a.holdEstimated || a.holdFallback || a.fanHeuristic)
        return true;
    }
    return false;
  }
};

namespace recipe_compiler {

// Seconds -> milliseconds as a saturating uint32 (guards a non-finite or absurd duration off the
// editor; the editor's NumericFieldConfig already bounds these, this is the backstop).
inline uint32_t secondsToMs(float seconds) {
  if (!(seconds > 0.0f)) // also catches NaN
    return 0;
  const double ms = static_cast<double>(seconds) * 1000.0;
  if (ms >= static_cast<double>(UINT32_MAX))
    return UINT32_MAX;
  return static_cast<uint32_t>(ms + 0.5);
}

// The channel fields common to a phase's ramp and hold segments.
inline oven_Segment baseSegment(const Phase &p, const FanDecision &fans) {
  oven_Segment seg = oven_Segment_init_default;
  seg.heat_c = p.targetC;
  seg.conv_fan = fans.convFan; // the only chamber fan; cooling is passive (§6)
  seg.uv = p.uv;
  seg.motor = p.motor;
  return seg;
}

} // namespace recipe_compiler

// Compile a phase list into a Recipe. `ambientC` is where the first phase's ramp starts from (the
// controller's real starting temp is measured, but the projected timeline/fan decisions need a
// nominal origin, §15). `id`/`seq` are stamped onto the Recipe for the setup path (§9).
inline CompileResult compileRecipe(const Phase *phases, size_t count, RecipeMode mode,
                                   const OvenModel &model, Caps caps, float ambientC, uint32_t id,
                                   uint32_t seq) {
  CompileResult r;
  r.recipe.id = id;
  r.recipe.seq = seq;
  r.recipe.mode = mode == RecipeMode::Cure ? oven_Mode_MODE_CURE : oven_Mode_MODE_REFLOW;
  r.uncalibratedPreview = !model.calibrated;
  r.phaseCount = count < kMaxPhases ? count : kMaxPhases;

  if (count == 0) {
    r.hardValid = false;
    r.reject = CompileReject::NoPhases;
    return r;
  }

  auto fail = [&](CompileReject why, size_t phase) {
    r.hardValid = false;
    r.reject = why;
    r.rejectPhase = phase;
    r.recipe.segments_count = 0;
    return r;
  };

  float prevC = ambientC;
  pb_size_t segCount = 0;

  for (size_t i = 0; i < count; ++i) {
    const Phase &p = phases[i];
    PhaseAdvisory adv;

    // --- Hard tier (mirrors RecipeValidator so an accepted compile never NAKs, §9) ---
    if (!std::isfinite(p.targetC))
      return fail(CompileReject::NonFiniteTarget, i);
    if (p.targetC > caps.capC || p.targetC < caps.minC)
      return fail(CompileReject::TargetOutOfRange, i);
    if (mode == RecipeMode::Reflow && (p.uv || p.motor))
      return fail(CompileReject::ModeContentMismatch, i);

    // --- Fan Auto resolution (B3) ---
    const FanContext fc{prevC, p.targetC, p.rampSeconds};
    const FanDecision fans = resolveFans(p.convFan, fc, model);
    adv.fanHeuristic = fans.heuristic;

    // --- Ramp segment (omitted when the phase makes no temperature change) ---
    if (p.targetC != prevC) {
      const bool heating = p.targetC > prevC;
      // Cooling is passive — no chamber cool fan (§6) — so the fan-off cool envelope always.
      const RateEnvelope &env = heating ? model.heat.pick(fans.convFan) : model.cool.off;
      oven_Segment seg = recipe_compiler::baseSegment(p, fans);
      if (p.rampSeconds <= 0.0f) {
        // ASAP: executed to target; dur_ms is the projected-duration estimate (ETA / watchdog, §5).
        seg.interp = oven_Interp_INTERP_RAMP_ASAP;
        seg.dur_ms = recipe_compiler::secondsToMs(rampDurationSeconds(env, prevC, p.targetC));
      } else {
        // Over-time: keep the requested sweep; the controller's hold-entry gate absorbs any lag, so
        // a physically-optimistic ramp is only flagged amber, not rewritten (§12).
        seg.interp = oven_Interp_INTERP_RAMP_OVER_TIME;
        seg.dur_ms = recipe_compiler::secondsToMs(p.rampSeconds);
        adv.rampRateLimited = rateLimitRamp(env, prevC, p.targetC, p.rampSeconds).rateLimited;
      }
      // Every emitted segment must carry dur_ms > 0 — the controller NAKs a zero-duration segment
      // (RecipeValidator). A sub-millisecond ramp rounds away to no segment at all, which is right.
      if (seg.dur_ms > 0) {
        if (segCount >= kMaxSegments)
          return fail(CompileReject::TooManySegments, i);
        r.segmentPhase[segCount] = static_cast<uint8_t>(i);
        r.recipe.segments[segCount++] = seg;
      }
    }

    // --- Hold segment ---
    float holdSeconds = 0.0f;
    if (mode == RecipeMode::Cure && p.uv && p.motor && model.beamCoverage > 0.0f) {
      // Dose authoring: hold seconds computed from UV-exposure-per-surface (§5). With only the
      // conservative-low placeholder beamCoverage (uncalibrated), the value is an estimate.
      holdSeconds = exposureToHoldSeconds(p.exposurePerSurface, model.beamCoverage);
      adv.holdEstimated = !model.calibrated;
    } else {
      holdSeconds = p.holdSeconds;
      // A cure phase that intended dose timing (uv+turntable/exposure) but had no coverage to
      // convert with fell back to raw seconds — surface that; a plain warm-up phase did not.
      adv.holdFallback =
          mode == RecipeMode::Cure && (p.uv || p.motor || p.exposurePerSurface > 0.0f);
    }
    const uint32_t holdMs = recipe_compiler::secondsToMs(holdSeconds);
    if (holdMs > 0) {
      oven_Segment seg = recipe_compiler::baseSegment(p, fans);
      seg.interp = oven_Interp_INTERP_HOLD;
      seg.dur_ms = holdMs;
      if (segCount >= kMaxSegments)
        return fail(CompileReject::TooManySegments, i);
      r.segmentPhase[segCount] = static_cast<uint8_t>(i);
      r.recipe.segments[segCount++] = seg;
    }

    if (i < kMaxPhases)
      r.phases[i] = adv;
    prevC = p.targetC;
  }

  // Implicit passive cool-down to a touch-safe temperature (§5/§6): every run that ends hotter than
  // kTouchSafeC gets an appended cool segment so the controller coasts the chamber down before it
  // reports Done. Its duration is the passive-coast time on the fan-off cool envelope (there is no
  // chamber cool fan) — the projected temp reaches kTouchSafeC at the segment's end. The controller
  // also runs its own independent backup cooldown, so this tail is a nicety, not the safety itself.
  if (needsImplicitCool(prevC)) {
    const uint32_t coolMs =
        recipe_compiler::secondsToMs(rampDurationSeconds(model.cool.off, prevC, kTouchSafeC));
    if (coolMs > 0) {
      oven_Segment seg = oven_Segment_init_default;
      seg.heat_c = kTouchSafeC;
      seg.conv_fan = false; // passive cool; the convection fan aids heating only (§6)
      seg.uv = false;
      seg.motor = false;
      seg.interp = oven_Interp_INTERP_RAMP_OVER_TIME;
      seg.dur_ms = coolMs;
      if (segCount >= kMaxSegments)
        return fail(CompileReject::TooManySegments, count - 1);
      r.segmentPhase[segCount] = kCoolSegment;
      r.recipe.segments[segCount++] = seg;
    }
  }

  // A phase list with no ramp and no hold anywhere produces nothing to run.
  if (segCount == 0)
    return fail(CompileReject::NoPhases, 0);

  r.recipe.segments_count = segCount;
  return r;
}
