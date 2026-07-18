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

#include "fan_resolver.h"  // resolveFans — the achievable curve honours fan-Auto (§5/§12, C5)
#include "implicit_cool.h" // the appended passive cool-down every run ends with (§6)
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
// a hold-end per phase — including the one implicit cool-down phase every run appends (§6). Lets
// callers stack-allocate.
inline constexpr size_t kMaxEffectivePhases = kMaxPhases + 1; // authored + implicit cool
inline constexpr size_t kMaxCurvePoints = kMaxEffectivePhases * 2 + 1;
// Upper bound on phase boundaries / phase-name labels a curve annotates — one per effective phase.
inline constexpr size_t kMaxCurvePhases = kMaxEffectivePhases;

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
// fan-conditioned by the phase's *resolved* intent — Auto is lowered to on/off via fan_resolver
// (§5), so the preview's achievable curve matches the fan state the compiled recipe would carry
// (recipe_compiler.h), rather than pessimistically treating Auto as off. rampSeconds is finite-
// guarded before it reaches the resolver (a raw blob may carry NaN, §7).
inline const RateEnvelope &rampEnvelope(const Phase &p, float fromT, float toT,
                                        const OvenModel &model) {
  const FanContext fc{fromT, toT, finiteOr(p.rampSeconds, 0.0f)};
  const FanDecision d = resolveFans(p.convFan, fc, model);
  // Cooling is passive — no chamber cool fan (§6) — so a falling ramp always uses the fan-off env.
  return toT >= fromT ? model.heat.pick(d.convFan) : model.cool.off;
}

// --- effective phase list (authored + implicit cool) ------------------------------------------

// Build the *effective* phase list the preview draws: the authored phases (clamped to kMaxPhases),
// followed by the implicit passive cool-down to kTouchSafeC when the run ends hotter than that
// (implicit_cool.h, §6). `dst` must hold kMaxEffectivePhases. Returns the effective count. This is
// the same tail recipe_compiler.h appends to the wire recipe, so the preview and the compiled run
// always agree on the cool-down. Robust to a raw Phase[] (targetC is finite-guarded to track the
// last setpoint).
inline size_t buildEffectivePhases(const Phase *phases, size_t count, const OvenModel &model,
                                   float ambientC, Phase *dst) {
  const size_t authored = count > kMaxPhases ? kMaxPhases : count;
  float lastT = clampT(ambientC);
  for (size_t i = 0; i < authored; ++i) {
    dst[i] = phases[i];
    lastT = clampT(finiteOr(phases[i].targetC, lastT));
  }
  if (needsImplicitCool(lastT)) {
    dst[authored] = implicitCoolPhase(lastT, model);
    return authored + 1;
  }
  return authored;
}

// Whether a run over this Phase[] ends with an implicit cool-down (i.e. its last setpoint is hotter
// than touch-safe). Lets a curve caller decide whether to append the "Cool" label parallel to the
// extra phase boundary the samplers emit. Uses the same finite-guarded last-setpoint rule as
// buildEffectivePhases, so caller and sampler always agree.
inline bool runHasImplicitCool(const Phase *phases, size_t count, const OvenModel &model,
                               float ambientC = kDefaultAmbientC) {
  (void)model;
  const size_t authored = count > kMaxPhases ? kMaxPhases : count;
  float lastT = clampT(ambientC);
  for (size_t i = 0; i < authored; ++i) {
    lastT = clampT(finiteOr(phases[i].targetC, lastT));
  }
  return needsImplicitCool(lastT);
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
  Phase eff[kMaxEffectivePhases];
  const size_t pc = buildEffectivePhases(phases, count, model, ambientC, eff);
  size_t n = 0;
  float t = 0.0f;
  float prevT = clampT(ambientC);
  out[n++] = CurvePoint{0.0f, prevT};

  for (size_t i = 0; i < pc && n + 2 <= cap; ++i) {
    const Phase &p = eff[i];
    const float endT = clampT(finiteOr(p.targetC, prevT));
    const RateEnvelope &env = rampEnvelope(p, prevT, endT, model);
    float req = finiteOr(p.rampSeconds, 0.0f);
    if (req < 0.0f) {
      req = 0.0f;
    }
    const RampFeasibility rf = rateLimitRamp(env, prevT, endT, req);
    // The phase occupies its PROJECTED duration on the shared time axis: the honored ramp time
    // (rf.achievableSeconds — the authored seconds when the plant can meet them, the plant-limited
    // time when the request is faster than physics) plus the hold. The oven does not advance to the
    // next phase until the projected trace actually reaches the setpoint and holds there (the real
    // control loop), so an ASAP ramp is NOT instantaneous — it takes the plant's real time. Both
    // traces share these slots, so they and the phase separators line up.
    const float rampAch = clampSec(rf.achievableSeconds);
    const float hold = clampSec(phaseHoldSeconds(mode, p, model));
    const float slotStart = t;
    if (achievable) {
      // Projected: rise/fall along the plant rate to the setpoint.
      t = clampSec(slotStart + rampAch);
    } else {
      // Requested (authored intent): reach the setpoint at the AUTHORED ramp time — instant for an
      // ASAP ramp (rampSeconds == 0), so the line rises vertically — then (below) sit at it until
      // the phase's projected end. The gap to the slower projected trace within the slot is the §12
      // feasibility divergence.
      const float reqRamp = p.rampSeconds > 0.0f ? clampSec(req) : 0.0f;
      t = clampSec(slotStart + reqRamp);
    }
    out[n++] = CurvePoint{t, endT};

    // Both traces hold the setpoint out to the phase's projected end (ramp + hold). For the
    // requested trace this also spans the wait while the projected trace is still catching up, so
    // both curves finish each phase — and the whole run — at the same time.
    const float slotEnd = clampSec(slotStart + rampAch + hold);
    if (slotEnd > t) {
      t = slotEnd;
      out[n++] = CurvePoint{t, endT};
    }
    prevT = endT;
  }
  return n;
}

// --- feasibility divergence + closed-loop preview (C5, §12) ------------------------------------

// True when any phase's requested over-time ramp is faster than the fan-resolved envelope can
// deliver — i.e. the requested and achievable curves pull apart (the §12 amber divergence). Mirrors
// recipe_compiler's PhaseAdvisory.rampRateLimited without building a wire recipe, so the read-only
// preview can flag it. ASAP ramps (rampSeconds ≤ 0) are never "too fast" by themselves.
inline bool anyRampRateLimited(const Phase *phases, size_t count, RecipeMode mode,
                               const OvenModel &model, float ambientC = kDefaultAmbientC) {
  (void)mode;
  const size_t pc = count > kMaxPhases ? kMaxPhases : count;
  float prevT = clampT(ambientC);
  for (size_t i = 0; i < pc; ++i) {
    const Phase &p = phases[i];
    const float endT = clampT(finiteOr(p.targetC, prevT));
    const float req = finiteOr(p.rampSeconds, 0.0f);
    if (req > 0.0f) {
      const RateEnvelope &env = rampEnvelope(p, prevT, endT, model);
      if (rateLimitRamp(env, prevT, endT, req).rateLimited) {
        return true;
      }
    }
    prevT = endT;
  }
  return false;
}

// A [start, end] time window (seconds) on the achievable timeline — used to shade the phases whose
// UV lamp is on (§12 cure preview).
struct TimeSpan {
  float start;
  float end;
};

// The time windows (achievable timeline) of the phases whose UV lamp is on — the preview shades
// these so the operator sees where the workpiece is being dosed. Cure only (reflow carries no UV,
// §4); returns 0 for reflow. Finite, ordered, bounded — safe on a raw Phase[]. Returns the count
// (<= kMaxPhases, <= cap).
inline size_t sampleUvSpans(const Phase *phases, size_t count, RecipeMode mode,
                            const OvenModel &model, float ambientC, TimeSpan *out, size_t cap) {
  if (out == nullptr || cap == 0 || mode != RecipeMode::Cure) {
    return 0;
  }
  const size_t pc = count > kMaxPhases ? kMaxPhases : count;
  size_t n = 0;
  float t = 0.0f;
  float prevT = clampT(ambientC);
  for (size_t i = 0; i < pc && n < cap; ++i) {
    const Phase &p = phases[i];
    const float endT = clampT(finiteOr(p.targetC, prevT));
    const RateEnvelope &env = rampEnvelope(p, prevT, endT, model);
    float req = finiteOr(p.rampSeconds, 0.0f);
    if (req < 0.0f) {
      req = 0.0f;
    }
    // Projected timeline (same slot width as samplePhaseBoundaries / the projected curve) so the UV
    // band lines up with the phase separators the operator sees.
    const float rampAch = clampSec(rateLimitRamp(env, prevT, endT, req).achievableSeconds);
    const float hold = clampSec(phaseHoldSeconds(mode, p, model));
    const float start = t;
    const float end = clampSec(t + rampAch + hold);
    if (p.uv) {
      out[n].start = start;
      out[n].end = end;
      ++n;
    }
    t = end;
    prevT = endT;
  }
  return n;
}

// The cumulative time (seconds) at the end of each phase on the PROJECTED timeline — the x
// positions of the phase separators the preview draws as vertical rules. A phase ends when the
// projected trace has reached its setpoint (the honored ramp time — authored when feasible,
// plant-limited when the request outruns physics) and held for the hold time; the next phase does
// not begin before that, so an ASAP ramp still occupies its real plant time rather than collapsing
// to a zero-width corner. Mirrors sampleCurve's slot layout and computeFacts' total, so the
// separators sit on the projected curve's corners and the last boundary equals the facts-line
// duration. Finite, monotonic non-decreasing, bounded — safe on a raw Phase[]. Returns the count
// (<= kMaxPhases, <= cap).
inline size_t samplePhaseBoundaries(const Phase *phases, size_t count, RecipeMode mode,
                                    const OvenModel &model, float ambientC, float *out,
                                    size_t cap) {
  if (out == nullptr || cap == 0) {
    return 0;
  }
  Phase eff[kMaxEffectivePhases];
  const size_t pc = buildEffectivePhases(phases, count, model, ambientC, eff);
  size_t n = 0;
  float t = 0.0f;
  float prevT = clampT(ambientC);
  for (size_t i = 0; i < pc && n < cap; ++i) {
    const Phase &p = eff[i];
    const float endT = clampT(finiteOr(p.targetC, prevT));
    const RateEnvelope &env = rampEnvelope(p, prevT, endT, model);
    float req = finiteOr(p.rampSeconds, 0.0f);
    if (req < 0.0f) {
      req = 0.0f;
    }
    // Projected ramp time (∫dT/rate, or the authored seconds when they are slower than the plant's
    // limit), the same slot width sampleCurve/computeFacts use — so the separator lands where the
    // projected trace turns the corner, and an ASAP cool leg gets its real cooling width.
    const float rampAch = clampSec(rateLimitRamp(env, prevT, endT, req).achievableSeconds);
    t = clampSec(t + rampAch + clampSec(phaseHoldSeconds(mode, p, model)));
    out[n++] = t;
    prevT = endT;
  }
  return n;
}

// Sample the closed-loop response: the *achievable* setpoint trajectory fed through the first-order
// {a,b,τ} lag (thermal_math::lpStep/boardTempEstimate), so the preview can show the rounded
// approach/soak-settling the oven really produces (§12's optional-later, now drawn). Emits exactly
// `cap` points (bounded work — no dependence on the profile's duration), each finite and in-bounds,
// with monotonic time; robust to a raw Phase[] (every step is finite-guarded, so a NaN setpoint
// cannot poison the running state). Returns the point count (0 if there is nothing to lag).
inline size_t sampleOvershoot(const Phase *phases, size_t count, RecipeMode mode,
                              const OvenModel &model, float ambientC, CurvePoint *out, size_t cap) {
  if (out == nullptr || cap < 2) {
    return 0;
  }
  CurvePoint sp[kMaxCurvePoints];
  const size_t ns =
      sampleCurve(phases, count, mode, model, /*achievable=*/true, ambientC, sp, kMaxCurvePoints);
  if (ns < 2) {
    return 0;
  }
  const float total = clampSec(sp[ns - 1].t);
  const LagParams &lag =
      model.lag.pick(false); // fan-off lag — an indicative, not per-phase, choice
  const size_t steps = cap;
  const float dt = steps > 1 ? total / static_cast<float>(steps - 1) : 0.0f;
  float lp = clampT(ambientC);
  size_t j = 0;
  for (size_t k = 0; k < steps; ++k) {
    const float frac = static_cast<float>(k) / static_cast<float>(steps - 1);
    const float t = clampSec(total * frac);
    while (j + 1 < ns && sp[j + 1].t < t) {
      ++j;
    }
    float setp = sp[ns - 1].T;
    if (j + 1 < ns) {
      const float t0 = sp[j].t;
      const float t1 = sp[j + 1].t;
      const float a = t1 > t0 ? clampf((t - t0) / (t1 - t0), 0.0f, 1.0f) : 0.0f;
      setp = sp[j].T + (sp[j + 1].T - sp[j].T) * a;
    }
    lp = finiteOr(lag.tau + dt > 0.0f ? lpStep(lp, setp, dt, lag.tau) : setp, setp);
    out[k] = CurvePoint{t, clampT(boardTempEstimate(lag, lp))};
  }
  return steps;
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
