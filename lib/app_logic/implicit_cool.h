// implicit_cool.h — every profile ends with an implicit passive cool-down to a touch-safe
// temperature (design.md §5, §6). The operator never authors it: the compiler appends it to the
// wire recipe so the controller runs it (recipe_compiler.h), and profile_facts appends it to the
// preview so the curve shows the cool-down (profile_facts.h). The controller ALSO enforces its own
// independent backup cooldown to the same threshold (control_logic / oven_safety.h) — belt and
// suspenders, since the compiled tail is advisory data the controller could be handed without.
//
// Its duration is the time the chamber takes to passively coast from the last authored setpoint
// down to kTouchSafeC on the fan-off cooling envelope (there is no chamber cool fan, §6) — so the
// projected chamber temperature is kTouchSafeC at the phase's end.
//
// Pure C++: no LVGL, no Arduino — header-only inline like the rest of the app_logic math.
#pragma once

#include "phase.h"
#include "thermal_math.h"
#include "touch_safe.h" // the ONE codebase-wide touch-safe temperature (oven_domain::kTouchSafeC)

// The touch-safe chamber temperature every run cools down to before it reports Done (§5/§6): cool
// enough that the operator can open the door and handle the workpiece. This name is the CYD-side
// spelling of the single shared constant (touch_safe.h); the controller reads the same source as
// oven_safety.h's TOUCH_SAFE_C for its independent backup cooldown, so the two cannot drift.
inline constexpr float kTouchSafeC = oven_domain::kTouchSafeC;

// The role label the preview shows for the appended cool-down phase (borrowed literal, never
// freed).
inline constexpr const char *kImplicitCoolLabel = "Cool";

// Does a run whose last authored setpoint is `lastTargetC` need an implicit cool-down? Only when it
// ends hotter than touch-safe (a run that already ends cool needs no tail).
inline bool needsImplicitCool(float lastTargetC) {
  return lastTargetC > kTouchSafeC;
}

// The synthetic cool-down phase appended after a run ending at `lastTargetC`: a timed ramp down to
// kTouchSafeC over the passive-coast duration (fan-off cool envelope), no hold, all channels off
// (heater off; cooling is passive and the convection fan aids heating only, §6). Callers gate on
// needsImplicitCool() first; when called on an already-cool run the ramp duration is simply 0.
inline Phase implicitCoolPhase(float lastTargetC, const OvenModel &model) {
  Phase c;
  c.targetC = kTouchSafeC;
  c.rampSeconds = needsImplicitCool(lastTargetC)
                      ? rampDurationSeconds(model.cool.off, lastTargetC, kTouchSafeC)
                      : 0.0f;
  c.holdSeconds = 0.0f;
  c.exposurePerSurface = 0.0f;
  c.uv = false;
  c.motor = false;
  c.convFan = FanMode::Off; // passive cool; the convection fan aids heating only (§6)
  return c;
}
