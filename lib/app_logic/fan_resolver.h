// fan_resolver.h — resolve a phase's tri-state fan intent to the concrete on/off the compiled
// Recipe carries (design.md §5 "Fan `Auto`"; backlog B3).
//
// `Auto` is resolved on the CYD at recipe-compile time so the controller stays a generic executor
// and no `Auto` value crosses the wire (§9). The decision is made against the fan-conditioned rate
// envelopes in oven_cal.h: turn conv_fan on when the requested heat ramp can't be met on the
// fan-off envelope, turn cool_fan on when the requested cool ramp is faster than passive cooling.
// Before any calibration exists (`!model.calibrated`) there are no real fan-on-vs-off rates to
// decide against, so `Auto` falls back to the simple heuristic "conv_fan on while heating, cool_fan
// on while cooling" (§5) and the decision is flagged so the preview can label it estimated (§12).
//
// The exact rate/target margins are an OPEN question (§10 "Fan `Auto` resolution"); they live here
// as named constants and will tighten against real envelopes. Pure C++: no LVGL, no Arduino —
// host-tested under native_logic_cyd.
#pragma once

#include "phase.h"
#include "thermal_math.h"

// Where a phase starts and where it is going — the context a fan decision needs.
struct FanContext {
  float fromC;       // phase start temp (previous phase's target, or ambient for the first)
  float toC;         // this phase's target
  float rampSeconds; // requested approach time; 0 = ASAP (as fast as possible)
};

// The resolved fan state for a phase, plus whether the resolution used the pre-calibration
// heuristic (a Auto fan decided without real envelopes → amber "estimated" in the preview).
struct FanDecision {
  bool convFan;
  bool coolFan;
  bool heuristic;
};

namespace fan_resolver {

// Fractional margin by which the fan-off envelope must fall short of the request before Auto turns
// a fan on — a small deadband so a ramp that is only marginally meetable without the fan does not
// flap on/off across recompiles. Placeholder pending §10's decision rule.
constexpr float kRampMarginFrac = 0.05f;

// Does the fan-off envelope fail to deliver a ramp of `requestedSeconds` from a→b? A non-positive
// request is ASAP (never "too slow" by itself — ASAP wants whichever envelope is faster, handled by
// the caller). `env` is the fan-off rate envelope for the direction of travel.
inline bool fanOffTooSlow(const RateEnvelope &offEnv, float a, float b, float requestedSeconds) {
  if (requestedSeconds <= 0.0f)
    return false;
  const float achievableOff = rampDurationSeconds(offEnv, a, b);
  return requestedSeconds < achievableOff * (1.0f - kRampMarginFrac);
}

} // namespace fan_resolver

// Resolve both fans for one phase. Explicit On/Off pass through untouched; only `Auto` consults the
// model. Returns the resolved booleans and a heuristic flag set when any Auto fan was decided on
// the uncalibrated fallback path.
inline FanDecision resolveFans(FanMode convMode, FanMode coolMode, const FanContext &ctx,
                               const OvenModel &model) {
  const bool heating = ctx.toC > ctx.fromC;
  const bool cooling = ctx.toC < ctx.fromC;
  const bool heuristic =
      !model.calibrated && (convMode == FanMode::Auto || coolMode == FanMode::Auto);

  FanDecision d{false, false, heuristic};

  // --- Convection fan: aids heating (rate + uniformity); irrelevant to passive cooling. ---
  switch (convMode) {
  case FanMode::On:
    d.convFan = true;
    break;
  case FanMode::Off:
    d.convFan = false;
    break;
  case FanMode::Auto:
    if (!model.calibrated) {
      d.convFan = heating; // heuristic: fan on while heating (§5)
    } else if (heating) {
      if (ctx.rampSeconds <= 0.0f) {
        // ASAP: pick whichever envelope reaches the target sooner.
        const float tOff = rampDurationSeconds(model.heat.off, ctx.fromC, ctx.toC);
        const float tOn = rampDurationSeconds(model.heat.on, ctx.fromC, ctx.toC);
        d.convFan = tOn < tOff;
      } else {
        d.convFan =
            fan_resolver::fanOffTooSlow(model.heat.off, ctx.fromC, ctx.toC, ctx.rampSeconds);
      }
    }
    break;
  }

  // --- Cooling fan: aids cooling only; forced convection beats passive cooling. ---
  switch (coolMode) {
  case FanMode::On:
    d.coolFan = true;
    break;
  case FanMode::Off:
    d.coolFan = false;
    break;
  case FanMode::Auto:
    if (!model.calibrated) {
      d.coolFan = cooling; // heuristic: fan on while cooling (§5)
    } else if (cooling) {
      if (ctx.rampSeconds <= 0.0f) {
        const float tOff = rampDurationSeconds(model.cool.off, ctx.fromC, ctx.toC);
        const float tOn = rampDurationSeconds(model.cool.on, ctx.fromC, ctx.toC);
        d.coolFan = tOn < tOff;
      } else {
        d.coolFan =
            fan_resolver::fanOffTooSlow(model.cool.off, ctx.fromC, ctx.toC, ctx.rampSeconds);
      }
    }
    break;
  }

  return d;
}
