// calibration_sweep.h — the planned calibration-sweep generator (design.md §5/§7/§20, backlog B9).
//
// Produces the characterization runs behind oven_cal.h (§6): the fan-conditioned heat-rate
// envelopes heatRate(T, conv_fan), the passive cool-rate coolRate(T), the {a,b,τ}(conv_fan)
// board-temp lag, and the duty_ss(T) feedforward curve. All of it is fit OFFLINE on a PC from the
// SD logs (§7/§20); this header only generates the profiles the oven runs to produce that data.
//
// WHY A PLANNED SWEEP, NOT RANDOM PROFILES. An earlier design DECIDED a *random*-profile generator
// (n×m random profiles). Thermal-plant system-identification practice is decisively against that
// for this oven: it is slow, dominantly first-order-plus-dead-time, and nonlinear in a
// *predictable, temperature-indexed* way (convective loss ∝ ΔT, radiative ∝ T⁴). The standard
// method is PLANNED step/decay tests at several operating points, then gain-schedule — which maps
// directly onto the quantities oven_cal.h wants. Random *setpoint* profiles run closed-loop, so the
// PID cancels the excitation and shapes the plant input; the legitimate randomized method is a
// *designed* PRBS on the heater DUTY, open-loop (§5's parked "open-loop duty mode gives cleaner
// identifiability" note), not random setpoints. So the sweep is deterministic and structured:
//
//   for each fan state: ASAP-ramp→band₀, hold, ASAP-ramp→band₁, hold, … (a staircase run)
//   plus dedicated cool-only runs (ramp to the top band, brief hold, coast)
//
// and every run is an ORDINARY plain-heat REFLOW profile through the normal recipe/executor/PID
// path — no new segment or actuator type. The data falls out for free:
//   - heatRate(T, fan): an ASAP ramp saturates the PID to ~100% duty → the max-rate envelope,
//     sampled across T over the ramp's saturated bulk (well-separated bands keep it saturated; the
//     setpoint_shaper only tapers duty in the final approach, and duty_ss comes from the holds);
//   - duty_ss(T, fan): the duty the PID settles to during each hold IS duty_ss;
//   - coolRate(T): every run ends with the compiler's implicit passive cool-down to touch-safe
//     (recipe_compiler.h) — a logged decay for free — and the cool-only runs add more (§20);
//   - {a,b,τ}(fan): the wall-vs-workpiece TC lag on every ramp/hold.
//
// SAFETY (airtight by construction; the fuzz differential proves it): every emitted run compiles
// hardValid and is accepted by the controller's RecipeValidator — targets are clamped into the
// passed-in Caps (the call site derives them, exactly as recipe_compiler does; this header never
// reaches into oven_safety.h); no run asserts uv/motor, so content-derived mode is REFLOW (the
// higher cap) and the content hard-max is never exceeded; every hold is floored > 0 so each phase
// emits a segment; phase count ≤ kMaxBands ≪ the 32-segment budget.
//
// The grid numbers (bands, hold time, cool repeats per scope) are §10-OPEN placeholders — tuned
// against real runs later (§8 step 4), the same convention as remainder.h's kPhaseDoneFrac and the
// executor watchdog constants. No runtime consumer is wired yet (the §20 wizard doesn't exist); B9
// ships as a standalone logic utility validated by its test + fuzz suites.
//
// Pure C++ over phase.h/profile_draft.h/recipe_compiler.h (for Caps) — no LVGL, no Arduino, no
// clock, no RNG (fully deterministic). Host-tested under native_logic_cyd.
#pragma once

#include <cstddef>

#include "phase.h"
#include "profile_draft.h"
#include "recipe_compiler.h" // Caps (the same struct the real compile call site passes)

namespace cal_sweep {

// The most temperature bands a single staircase run walks. Well under kMaxPhases / the 32-segment
// wire budget (each band emits ≤2 segments + the implicit cool tail).
inline constexpr size_t kMaxBands = 4;

// A hold shorter than this rounds to a positive segment anyway; floor every hold to it so each
// phase always emits a hold segment (hardValid needs ≥1 segment / dur_ms > 0).
inline constexpr float kMinHoldSeconds = 1.0f;

// --- §10-open grid placeholders (design.md §10 "Calibration-sweep grid") ---------------------
inline constexpr float kNominalAmbientC = 22.0f;    // ramp origin for the projected timeline only
inline constexpr float kBandHoldSeconds = 300.0f;   // hold at each band so duty settles → duty_ss
inline constexpr float kCoolRunHoldSeconds = 20.0f; // brief hold before a dedicated cool decay

// The re-scoped §20 presets (design.md §20 step 2): Quick = cure-range only (≤120 °C); Standard =
// full-range heat + a passive cool; Thorough = adds fuller passive-cool characterization.
enum class Scope : uint8_t { Quick, Standard, Thorough };

// The concrete sweep a scope expands to. Bands are the *requested* setpoints; generateRun() drops
// any that fall outside the run's Caps, so this struct carries no policy about the caps.
struct Grid {
  float bands[kMaxBands] = {};
  size_t bandCount = 0;
  float holdSeconds = kBandHoldSeconds;
  float coolHoldSeconds = kCoolRunHoldSeconds;
  float ambientC = kNominalAmbientC;
  bool fanOff = true; // characterize with the convection fan off
  bool fanOn = true;  // and on — the fan is effectively a second plant (§5/§6)
  size_t coolOnlyRuns = 1;
};

inline Grid gridFor(Scope scope) {
  Grid g;
  switch (scope) {
  case Scope::Quick: // cure-range only (≤120 °C) — enough to run cure mode credibly
    g.bands[0] = 60.0f;
    g.bands[1] = 90.0f;
    g.bands[2] = 120.0f;
    g.bandCount = 3;
    g.coolOnlyRuns = 1;
    break;
  case Scope::Standard: // full-range heating + a passive cool
    g.bands[0] = 80.0f;
    g.bands[1] = 150.0f;
    g.bands[2] = 220.0f;
    g.bandCount = 3;
    g.coolOnlyRuns = 1;
    break;
  case Scope::Thorough: // adds fuller passive-cool characterization (multiple cools, §20)
    g.bands[0] = 80.0f;
    g.bands[1] = 150.0f;
    g.bands[2] = 220.0f;
    g.bandCount = 3;
    g.coolOnlyRuns = 3;
    break;
  }
  return g;
}

// How many staircase (fan-state) runs the sweep emits before the cool-only runs.
inline size_t fanRunCount(const Grid &g) {
  return static_cast<size_t>(g.fanOff ? 1 : 0) + static_cast<size_t>(g.fanOn ? 1 : 0);
}

// Total runs the sweep emits (staircases + dedicated cool decays). The caller runs them
// back-to-back as "run i of runCount", exactly as the §20 wizard will drive it.
inline size_t runCount(const Grid &g) {
  return fanRunCount(g) + g.coolOnlyRuns;
}

namespace detail {

// Bounded, NUL-terminated copy of a literal into a fixed name buffer.
inline void copyName(char *dst, size_t cap, const char *src) {
  size_t i = 0;
  for (; src[i] != '\0' && i + 1 < cap; ++i) {
    dst[i] = src[i];
  }
  dst[i] = '\0';
}

// "cal cool <idx>" — idx is the 1-based cool-run number.
inline void coolName(char *dst, size_t cap, size_t idx) {
  copyName(dst, cap, "cal cool ");
  size_t len = 0;
  while (dst[len] != '\0') {
    ++len;
  }
  char digits[8];
  size_t d = 0;
  do {
    digits[d++] = static_cast<char>('0' + idx % 10);
    idx /= 10;
  } while (idx > 0 && d < sizeof(digits));
  for (size_t j = 0; j < d && len + 1 < cap; ++j) {
    dst[len++] = digits[d - 1 - j];
  }
  dst[len] = '\0';
}

// Collect the grid bands that fall inside [caps.minC, caps.capC], ascending, into `out`. Returns
// the count (0 for degenerate caps — NaN/inverted range — or when no band fits, so the caller
// refuses).
inline size_t usableBands(const Grid &g, Caps caps, float *out) {
  if (!(caps.capC >= caps.minC)) { // catches NaN caps and an inverted range
    return 0;
  }
  size_t n = 0;
  for (size_t i = 0; i < g.bandCount && i < kMaxBands; ++i) {
    const float b = g.bands[i];
    if (b >= caps.minC && b <= caps.capC) {
      out[n++] = b;
    }
  }
  return n;
}

inline float floorHold(float s) {
  return s > kMinHoldSeconds ? s : kMinHoldSeconds;
}

} // namespace detail

// Emit run `i` of the sweep as a plain-heat REFLOW ProfileDraft. Deterministic: the same
// (grid, i, caps) always yields the same draft. The result is always hardValid within `caps`.
// Returns false iff i >= runCount(grid) or no band fits the caps (degenerate/too-tight caps) —
// refusing is a valid answer, mirroring cure_resume::build. `out` is written only on success.
inline bool generateRun(const Grid &g, size_t i, Caps caps, ProfileDraft &out) {
  if (i >= runCount(g)) {
    return false;
  }

  float bands[kMaxBands];
  const size_t nb = detail::usableBands(g, caps, bands);
  if (nb == 0) {
    return false; // no requested band is inside the caps
  }

  out = ProfileDraft{};
  out.mode = RecipeMode::Reflow; // plain heat, never uv/motor → content-derived REFLOW cap
  out.stock = false;

  const size_t fanRuns = fanRunCount(g);
  if (i < fanRuns) {
    // A staircase run at one fan state: ASAP ramp + settling hold at each usable band. Fan order is
    // off first (if present), then on.
    const bool fanOn = (g.fanOff && g.fanOn) ? (i == 1) : g.fanOn;
    const FanMode fan = fanOn ? FanMode::On : FanMode::Off;
    detail::copyName(out.name, kProfileNameCap, fanOn ? "cal heat fan-on" : "cal heat fan-off");
    for (size_t k = 0; k < nb; ++k) {
      Phase &p = out.phases[k];
      detail::copyName(p.name, kPhaseNameCap, "heat");
      p.targetC = bands[k];
      p.rampSeconds = 0.0f; // ASAP → PID saturates → heatRate(T, fan) envelope
      p.holdSeconds = detail::floorHold(g.holdSeconds); // hold → duty_ss(T, fan)
      p.exposurePerSurface = 0.0f;
      p.uv = false;
      p.motor = false;
      p.convFan = fan;
    }
    out.phaseCount = nb;
  } else {
    // A dedicated cool-only decay: ramp to the top usable band, brief hold, then the compiler's
    // implicit passive cool-down logs the coast. Fan off — cooling is passive (§6).
    const size_t k = i - fanRuns; // 0-based cool index
    Phase &p = out.phases[0];
    detail::copyName(p.name, kPhaseNameCap, "cool");
    p.targetC = bands[nb - 1];
    p.rampSeconds = 0.0f;
    p.holdSeconds = detail::floorHold(g.coolHoldSeconds);
    p.exposurePerSurface = 0.0f;
    p.uv = false;
    p.motor = false;
    p.convFan = FanMode::Off;
    out.phaseCount = 1;
    detail::coolName(out.name, kProfileNameCap, k + 1);
  }
  return true;
}

} // namespace cal_sweep
