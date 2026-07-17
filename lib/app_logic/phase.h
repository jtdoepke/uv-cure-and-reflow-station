// phase.h — the CYD's editable domain model for a profile (design.md §5, §12; backlog B1).
//
// The operator does not author generic segments; they author a list of uniform three-number
// *phases* — target temp, ramp seconds `x` (x = 0 = "as fast as possible"), hold `y` — plus
// per-phase channels. recipe_compiler.h lowers a Phase[] into the wire `oven_Segment[]` the
// controller executes; fan_resolver.h turns the tri-state fan intent into the resolved on/off the
// compiled Recipe carries (no `Auto` crosses the wire, §5).
//
// This is the shared struct C5's editor mutates in place, so it is kept apart from the compiler
// (which only reads it). Pure C++: no LVGL, no Arduino — host-tested under native_logic_cyd.
#pragma once

#include <cstddef>
#include <cstdint>

// Per-phase convection-fan intent. The default is `Auto`, resolved to on/off at recipe-compile time
// against oven_cal.h (fan_resolver.h). `uv`/`motor` stay plain on/off — only the convection fan is
// tri-state (§5 "Fan `Auto`"). There is no cooling fan (the teardown found none, §6).
enum class FanMode : uint8_t { Auto = 0, On = 1, Off = 2 };

// Which mode a whole profile is authored in. Selects the hold-authoring convention (cure holds are
// UV-exposure-per-surface, reflow holds are raw seconds — §5) and the content/tag consistency the
// compiler enforces. Distinct from the wire `oven_Mode`: this is the editor's domain enum, the
// compiler stamps the matching `oven_Mode` tag on the Recipe.
enum class RecipeMode : uint8_t { Cure, Reflow };

// A profile carries at most this many phases. Matches the compiler's 32-segment wire budget
// (recipe_compiler.h / oven.options): each phase emits >=1 segment, so a longer list could never
// upload. Lives here — the Phase-domain fact — so ProfileStore (B4) can size its stored array to it
// without pulling in the compiler/protobuf; recipe_compiler.h static_asserts it against
// kMaxSegments.
inline constexpr size_t kMaxPhases = 32;

// One authored phase. A phase compiles to up to two segments (a ramp toward `targetC`, then a hold
// at it); a degenerate ramp (no temperature change) or a non-positive hold is omitted to conserve
// the 32-segment wire budget (oven.options).
struct Phase {
  float targetC = 0.0f;     // phase setpoint, deg C
  float rampSeconds = 0.0f; // approach time; 0 => RAMP_ASAP (drive to target, controller-gated §5)

  // Hold authoring is mode-dependent (§5):
  //   - reflow: holdSeconds is the raw soak time.
  //   - cure: exposurePerSurface (UV dose) is authored; the compiler computes hold seconds via
  //     beamCoverage when uv + motor are on and the model is calibrated, else falls back to
  //     holdSeconds (turntable off / pre-calibration).
  float holdSeconds = 0.0f;
  float exposurePerSurface = 0.0f; // cure only; ignored in reflow

  bool uv = false;    // UV lamp on for this phase (cure)
  bool motor = false; // turntable on for this phase (cure)

  // Convection fan only — the teardown found no chamber cooling fan (§6), so cooling is passive and
  // there is no cool-fan channel. The electronics cooling fan is always on and controller-managed,
  // not a per-phase setting.
  FanMode convFan = FanMode::Auto;
};
