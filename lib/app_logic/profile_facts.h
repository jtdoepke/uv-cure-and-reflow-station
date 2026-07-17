// profile_facts.h — the derived facts the profile library (C4) and its curve preview render from a
// stored profile's Phase[] (design.md §12, §15, §23; backlog C4).
//
// Two jobs, both pure over the shared thermal math (thermal_math.h):
//   - computeFacts(): the row/detail facts — peak setpoint and an estimated end-to-end duration
//     (∫dT/rate ASAP-ramp integral + holds), plus the phase count. Feeds §23's "peak 245° · ~6:10".
//   - sampleCurve(): the temperature-vs-time polyline the read-only §12 preview draws — a
//     *requested* trajectory (what was authored) and an *achievable* one (requested rate-limited by
//     the calibrated envelope). This is the minimal first cut of the C5 feasibility widget; C5
//     extends it to amber divergence flags + the closed-loop {a,b,τ} overshoot. Keeping the point
//     computation here (not in the widget) is what lets C5 reuse it.
//
// Robust to a RAW, pre-validation Phase[] on purpose: a loaded profile can be an untrusted blob
// pushed over serial/WiFi (§7), and ProfileStore bounds phaseCount + the name but NOT the Phase
// float fields — so targetC/rampSeconds/holdSeconds/exposurePerSurface arrive here as NaN/Inf/huge.
// Every input float is finite-guarded and clamped to a declared bound (constrained, not
// validated-after — the NumericEntry/ProfileExecutor idiom), so peak/total and every emitted
// CurvePoint are always finite and within [kTempLo,kTempHi] × [0,kMaxSeconds]. That guarantee is
// what lets the LVGL curve widget stay dumb (it only scales already-bounded points), and it is
// pinned by fuzz/fuzz_profile_facts.cpp.
//
// Pure C++: no LVGL, no Arduino — host-tested under native_logic_cyd; header-only inline like the
// rest of the app_logic math (thermal_math.h / recipe_compiler.h).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "phase.h"
#include "thermal_math.h"

namespace profile_facts {

// Declared output bounds — every sampled/derived value is clamped into these, so a NaN/Inf/huge
// stored float can never reach the widget's coordinate scaling or the formatters.
inline constexpr float kTempLo = -100.0f;    // °C
inline constexpr float kTempHi = 10000.0f;   // °C — far above any real setpoint, still finite
inline constexpr float kMaxSeconds = 1.0e7f; // ~115 days — a bound, not a real runtime

// A default ambient the curve/ETA start from when the caller has no live reading (the library
// computes facts off a saved profile, not a running oven).
inline constexpr float kDefaultAmbientC = 25.0f;

// A sampled point on the temperature-vs-time curve, in raw units (the widget scales to pixels).
struct CurvePoint {
  float t; // seconds from start
  float T; // °C
};

// Upper bound on points a sampled curve emits: the initial ambient point, then up to a ramp-end and
// a hold-end per phase. Lets callers stack-allocate.
inline constexpr size_t kMaxCurvePoints = kMaxPhases * 2 + 1;

struct ProfileFacts {
  float peakC = 0.0f;        // hottest authored setpoint (finite; 0 when there are no phases)
  float totalSeconds = 0.0f; // estimated end-to-end duration (finite)
  uint16_t phaseCount = 0;
};

// --- finite / range guards --------------------------------------------------------------------

// Map a non-finite value (NaN, ±Inf) to `fallback`; pass finite values through. No <cmath>: the
// self-inequality catches NaN, the magnitude test catches ±Inf.
inline float finiteOr(float v, float fallback) {
  if (v != v) {
    return fallback; // NaN
  }
  if (v > 3.4e38f || v < -3.4e38f) {
    return fallback; // ±Inf
  }
  return v;
}

inline float clampT(float v) {
  return clampf(finiteOr(v, 0.0f), kTempLo, kTempHi);
}
inline float clampSec(float v) {
  return clampf(finiteOr(v, 0.0f), 0.0f, kMaxSeconds);
}

// A fan is engaged for the preview only when explicitly On; Auto resolves to off here (the
// conservative, slower envelope). Full fan-Auto resolution (fan_resolver.h) is C5's; this minimal
// cut picks the plainer envelope rather than reaching into the resolver.
inline bool fanEngaged(FanMode m) {
  return m == FanMode::On;
}

// The hold seconds a phase contributes: reflow = raw soak; cure = UV-exposure-per-surface converted
// via beamCoverage, falling back to raw seconds when there is no coverage (turntable off /
// uncalibrated) — mirrors recipe_compiler.h's rule.
inline float phaseHoldSeconds(RecipeMode mode, const Phase &p, const OvenModel &model) {
  if (mode == RecipeMode::Cure) {
    const float h = exposureToHoldSeconds(finiteOr(p.exposurePerSurface, 0.0f), model.beamCoverage);
    if (h > 0.0f) {
      return h;
    }
  }
  return finiteOr(p.holdSeconds, 0.0f);
}

// The rate envelope a ramp from `fromT` to `toT` runs against (rising → heat, falling → cool),
// fan-conditioned by the phase's intent.
inline const RateEnvelope &rampEnvelope(const Phase &p, float fromT, float toT,
                                        const OvenModel &model) {
  return toT >= fromT ? model.heat.pick(fanEngaged(p.convFan))
                      : model.cool.pick(fanEngaged(p.coolFan));
}

// --- facts ------------------------------------------------------------------------------------

// Peak setpoint + estimated total duration for a stored profile. Duration uses the *achievable*
// ramp time (∫dT/rate) plus the hold, so it matches the run's ETA rather than the authored ramp.
inline ProfileFacts computeFacts(const Phase *phases, size_t count, RecipeMode mode,
                                 const OvenModel &model, float ambientC = kDefaultAmbientC) {
  ProfileFacts f{};
  const size_t pc = count > kMaxPhases ? kMaxPhases : count;
  f.phaseCount = static_cast<uint16_t>(pc);

  bool anyPeak = false;
  float peak = 0.0f;
  float total = 0.0f;
  float prevT = clampT(ambientC);
  for (size_t i = 0; i < pc; ++i) {
    const Phase &p = phases[i];
    const float endT = clampT(finiteOr(p.targetC, prevT));
    if (!anyPeak || endT > peak) {
      peak = endT;
      anyPeak = true;
    }
    const RateEnvelope &env = rampEnvelope(p, prevT, endT, model);
    float req = finiteOr(p.rampSeconds, 0.0f);
    if (req < 0.0f) {
      req = 0.0f;
    }
    const RampFeasibility rf = rateLimitRamp(env, prevT, endT, req);
    total = clampSec(total + clampSec(rf.achievableSeconds) + phaseHoldSeconds(mode, p, model));
    prevT = endT;
  }
  f.peakC = anyPeak ? peak : 0.0f;
  f.totalSeconds = total;
  return f;
}

// --- curve sampling ---------------------------------------------------------------------------

// Fill `out` with the temperature-vs-time polyline. `achievable == false` draws the *requested*
// trajectory (authored ramp seconds; an ASAP ramp has no authored time so it uses the achievable
// one); `achievable == true` draws the requested trajectory rate-limited to what the plant can do.
// Returns the number of points written (<= cap, <= kMaxCurvePoints). Every point is finite and
// within the declared bounds.
inline size_t sampleCurve(const Phase *phases, size_t count, RecipeMode mode,
                          const OvenModel &model, bool achievable, float ambientC, CurvePoint *out,
                          size_t cap) {
  if (out == nullptr || cap == 0) {
    return 0;
  }
  const size_t pc = count > kMaxPhases ? kMaxPhases : count;
  size_t n = 0;
  float t = 0.0f;
  float prevT = clampT(ambientC);
  out[n++] = CurvePoint{0.0f, prevT};

  for (size_t i = 0; i < pc && n + 2 <= cap; ++i) {
    const Phase &p = phases[i];
    const float endT = clampT(finiteOr(p.targetC, prevT));
    const RateEnvelope &env = rampEnvelope(p, prevT, endT, model);
    float req = finiteOr(p.rampSeconds, 0.0f);
    if (req < 0.0f) {
      req = 0.0f;
    }
    const RampFeasibility rf = rateLimitRamp(env, prevT, endT, req);
    // Requested time = the authored ramp seconds (ASAP has none → use achievable); achievable time
    // = the rate-limited result.
    const float rampSec =
        achievable ? rf.achievableSeconds : (p.rampSeconds > 0.0f ? req : rf.achievableSeconds);
    t = clampSec(t + clampSec(rampSec));
    out[n++] = CurvePoint{t, endT};

    const float hold = clampSec(phaseHoldSeconds(mode, p, model));
    if (hold > 0.0f) {
      t = clampSec(t + hold);
      out[n++] = CurvePoint{t, endT};
    }
    prevT = endT;
  }
  return n;
}

// --- formatting -------------------------------------------------------------------------------

// "~6:10" (m:ss) or "~1:02:03" (h:mm:ss) for a long profile. Always fits `out`; guards a
// non-finite/huge input by clamping first.
inline void formatDuration(float seconds, char *out, size_t n) {
  if (out == nullptr || n == 0) {
    return;
  }
  const uint32_t total = static_cast<uint32_t>(clampf(finiteOr(seconds, 0.0f), 0.0f, kMaxSeconds));
  const uint32_t hh = total / 3600u;
  const uint32_t mm = (total % 3600u) / 60u;
  const uint32_t ss = total % 60u;
  if (hh > 0u) {
    std::snprintf(out, n, "~%u:%02u:%02u", hh, mm, ss);
  } else {
    std::snprintf(out, n, "~%u:%02u", mm, ss);
  }
}

// "peak 245°" (in °C, or °F when `fahrenheit`). Guards a non-finite input.
inline void formatPeak(float peakC, bool fahrenheit, char *out, size_t n) {
  if (out == nullptr || n == 0) {
    return;
  }
  const float c = clampT(peakC);
  const float v = fahrenheit ? c * 9.0f / 5.0f + 32.0f : c;
  const int iv = static_cast<int>(v >= 0.0f ? v + 0.5f : v - 0.5f);
  std::snprintf(out, n, "peak %d\xC2\xB0", iv); // \xC2\xB0 = UTF-8 ° (carried by the fonts)
}

} // namespace profile_facts
