// native_logic_cyd suite — pure host tests of profile_facts.h: the peak/duration facts and the
// requested/achievable curve sampling the profile library (C4) and its §12 preview render, plus the
// duration/peak formatters. No LVGL, no Arduino. Backlog C4 (design.md §12/§15/§23).
//
// The robustness half (finite/bounded output on a raw, pre-validation Phase[]) is pinned
// exhaustively by fuzz/fuzz_profile_facts.cpp; here we assert the spot-check invariants and the
// arithmetic.
#include <cmath>
#include <cstring>

#include <unity.h>

#include "oven_cal.h"
#include "profile_facts.h"

using profile_facts::computeFacts;
using profile_facts::CurvePoint;
using profile_facts::formatDuration;
using profile_facts::formatPeak;
using profile_facts::kMaxCurvePoints;
using profile_facts::kMaxSeconds;
using profile_facts::kTempHi;
using profile_facts::kTempLo;
using profile_facts::ProfileFacts;
using profile_facts::sampleCurve;

void setUp(void) {}
void tearDown(void) {}

// A constant-rate toy model so the maths is exact: heat 2 °C/s, cool 1 °C/s, no fan variation, no
// beam coverage. `calibrated` is irrelevant to the facts (it only gates C5's amber labeling).
static OvenModel toyModel() {
  RateEnvelope heat{0.0f, 2.0f, 0.05f, 2.0f};
  RateEnvelope cool{0.0f, 1.0f, 0.05f, 1.0f};
  LagParams lag{1.0f, 0.0f, 30.0f};
  DutyModel duty{0.0f, 0.1f, 1.0f};
  return OvenModel{{heat, heat},          {cool, cool},          {lag, lag},         {duty, duty},
                   /*beamCoverage=*/0.5f, /*turntableRpm=*/5.0f, /*calibrated=*/true};
}

// --- facts -------------------------------------------------------------------------------------

void test_facts_peak_is_hottest_setpoint(void) {
  const OvenModel m = toyModel();
  Phase phases[3] = {};
  phases[0].targetC = 100.0f;
  phases[1].targetC = 245.0f; // the peak
  phases[2].targetC = 60.0f;
  const ProfileFacts f = computeFacts(phases, 3, RecipeMode::Reflow, m, /*ambientC=*/25.0f);
  TEST_ASSERT_EQUAL_UINT16(3, f.phaseCount);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 245.0f, f.peakC);
}

void test_facts_duration_ramp_plus_hold(void) {
  const OvenModel m = toyModel();
  // One reflow phase: 25 → 125 °C at 2 °C/s (achievable = 50 s) + a 60 s hold. rampSeconds = 0
  // (ASAP), so the estimate uses the achievable ramp.
  Phase p = {};
  p.targetC = 125.0f;
  p.rampSeconds = 0.0f;
  p.holdSeconds = 60.0f;
  const ProfileFacts f = computeFacts(&p, 1, RecipeMode::Reflow, m, /*ambientC=*/25.0f);
  TEST_ASSERT_FLOAT_WITHIN(0.2f, 110.0f, f.totalSeconds); // 50 s ramp + 60 s hold
}

void test_facts_empty_profile_is_zero(void) {
  const OvenModel m = toyModel();
  const ProfileFacts f = computeFacts(nullptr, 0, RecipeMode::Reflow, m);
  TEST_ASSERT_EQUAL_UINT16(0, f.phaseCount);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, f.peakC);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, f.totalSeconds);
}

void test_facts_cure_hold_from_exposure(void) {
  const OvenModel m = toyModel();
  // Cure phase authored by UV dose: exposurePerSurface = 30, beamCoverage 0.5 → hold 60 s.
  Phase p = {};
  p.targetC = 60.0f;
  p.rampSeconds = 0.0f; // ASAP: 25→60 at 2 °C/s = 17.5 s
  p.exposurePerSurface = 30.0f;
  p.uv = true;
  p.motor = true;
  const ProfileFacts f = computeFacts(&p, 1, RecipeMode::Cure, m, /*ambientC=*/25.0f);
  TEST_ASSERT_FLOAT_WITHIN(0.3f, 17.5f + 60.0f, f.totalSeconds);
}

// --- curve sampling ----------------------------------------------------------------------------

void test_curve_starts_at_ambient_and_is_monotonic_in_time(void) {
  const OvenModel m = toyModel();
  Phase phases[3] = {};
  phases[0].targetC = 150.0f;
  phases[0].holdSeconds = 30.0f;
  phases[1].targetC = 245.0f;
  phases[1].holdSeconds = 20.0f;
  phases[2].targetC = 100.0f; // a cool-down leg
  CurvePoint pts[kMaxCurvePoints];
  const size_t n = sampleCurve(phases, 3, RecipeMode::Reflow, m, /*achievable=*/true,
                               /*ambientC=*/25.0f, pts, kMaxCurvePoints);
  TEST_ASSERT_TRUE(n >= 2);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, pts[0].t);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 25.0f, pts[0].T);
  for (size_t i = 1; i < n; ++i) {
    TEST_ASSERT_TRUE(pts[i].t >= pts[i - 1].t); // time never goes backwards
  }
  // The hottest point equals the peak setpoint.
  float hottest = pts[0].T;
  for (size_t i = 1; i < n; ++i) {
    if (pts[i].T > hottest) {
      hottest = pts[i].T;
    }
  }
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 245.0f, hottest);
}

void test_curve_requested_vs_achievable_diverge_when_rate_limited(void) {
  const OvenModel m = toyModel();
  // Ask for 25 → 225 °C (achievable 100 s at 2 °C/s) in an impossible 10 s. The requested curve
  // reaches target in 10 s; the achievable one takes the full 100 s — the within-phase divergence,
  // even though both traces occupy the same projected phase slot and finish together.
  Phase p = {};
  p.targetC = 225.0f;
  p.rampSeconds = 10.0f;
  CurvePoint req[kMaxCurvePoints];
  CurvePoint ach[kMaxCurvePoints];
  const size_t nr = sampleCurve(&p, 1, RecipeMode::Reflow, m, false, 25.0f, req, kMaxCurvePoints);
  const size_t na = sampleCurve(&p, 1, RecipeMode::Reflow, m, true, 25.0f, ach, kMaxCurvePoints);
  TEST_ASSERT_TRUE(nr >= 2 && na >= 2);
  // Index 1 is the end of the single authored ramp (index 0 is ambient); a passive cool-down tail
  // to touch-safe (§6) follows as further points, so assert on the phase corner, not the last
  // point.
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 10.0f, req[1].t);  // requested: honored 10 s
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 100.0f, ach[1].t); // achievable: rate-limited to 100 s
}

void test_curve_bounded_under_nonfinite_input(void) {
  const OvenModel m = toyModel();
  Phase phases[2] = {};
  phases[0].targetC = std::nanf(""); // NaN target
  phases[0].rampSeconds = INFINITY;  // Inf ramp
  phases[1].targetC = 1.0e30f;       // absurd but finite
  phases[1].holdSeconds = -50.0f;    // negative hold
  CurvePoint pts[kMaxCurvePoints];
  const size_t n = sampleCurve(phases, 2, RecipeMode::Reflow, m, true, 25.0f, pts, kMaxCurvePoints);
  for (size_t i = 0; i < n; ++i) {
    TEST_ASSERT_TRUE(std::isfinite(pts[i].t));
    TEST_ASSERT_TRUE(std::isfinite(pts[i].T));
    TEST_ASSERT_TRUE(pts[i].t >= 0.0f && pts[i].t <= kMaxSeconds);
    TEST_ASSERT_TRUE(pts[i].T >= kTempLo && pts[i].T <= kTempHi);
  }
}

// --- C5 feasibility extensions -----------------------------------------------------------------

// A model where the convection fan genuinely speeds heating (off 1 °C/s, on 4 °C/s), so fan-Auto
// resolution changes the achievable rate — the thing the C5 upgrade to rampEnvelope enables.
static OvenModel fanModel() {
  RateEnvelope heatOff{0.0f, 1.0f, 0.05f, 1.0f};
  RateEnvelope heatOn{0.0f, 4.0f, 0.05f, 4.0f};
  RateEnvelope cool{0.0f, 1.0f, 0.05f, 1.0f};
  LagParams lag{1.0f, 0.0f, 30.0f};
  DutyModel duty{0.0f, 0.1f, 1.0f};
  return OvenModel{{heatOff, heatOn},  {cool, cool}, {lag, lag}, {duty, duty}, 0.5f, 5.0f,
                   /*calibrated=*/true};
}

void test_achievable_curve_resolves_fan_auto(void) {
  const OvenModel m = fanModel();
  // 25 → 105 °C (ΔT 80), ASAP, conv fan Auto. Resolution picks the faster fan-on envelope (4 °C/s →
  // 20 s), not the pessimistic fan-off one (1 °C/s → 80 s) the old minimal cut used.
  Phase p = {};
  p.targetC = 105.0f;
  p.rampSeconds = 0.0f;
  p.convFan = FanMode::Auto;
  CurvePoint ach[kMaxCurvePoints];
  const size_t na = sampleCurve(&p, 1, RecipeMode::Reflow, m, true, 25.0f, ach, kMaxCurvePoints);
  TEST_ASSERT_TRUE(na >= 2);
  // ach[1] is the end of the authored ASAP ramp (a passive cool-down tail follows, §6).
  TEST_ASSERT_FLOAT_WITHIN(1.0f, 20.0f, ach[1].t);
}

void test_any_ramp_rate_limited(void) {
  const OvenModel m = toyModel();
  Phase fast = {};
  fast.targetC = 225.0f;    // 25→225 achievable 100 s at 2 °C/s
  fast.rampSeconds = 10.0f; // asked in 10 s → limited
  TEST_ASSERT_TRUE(profile_facts::anyRampRateLimited(&fast, 1, RecipeMode::Reflow, m, 25.0f));
  Phase slow = {};
  slow.targetC = 125.0f;
  slow.rampSeconds = 1000.0f; // plenty of time
  TEST_ASSERT_FALSE(profile_facts::anyRampRateLimited(&slow, 1, RecipeMode::Reflow, m, 25.0f));
}

void test_overshoot_bounded_and_monotonic(void) {
  const OvenModel m = toyModel();
  Phase phases[2] = {};
  phases[0].targetC = 150.0f;
  phases[0].holdSeconds = 60.0f;
  phases[1].targetC = 50.0f;
  CurvePoint ov[kMaxCurvePoints];
  const size_t n =
      profile_facts::sampleOvershoot(phases, 2, RecipeMode::Reflow, m, 25.0f, ov, kMaxCurvePoints);
  TEST_ASSERT_TRUE(n >= 2);
  for (size_t i = 0; i < n; ++i) {
    TEST_ASSERT_TRUE(std::isfinite(ov[i].t) && std::isfinite(ov[i].T));
    TEST_ASSERT_TRUE(ov[i].t >= 0.0f && ov[i].t <= kMaxSeconds);
    TEST_ASSERT_TRUE(ov[i].T >= kTempLo && ov[i].T <= kTempHi);
    if (i > 0) {
      TEST_ASSERT_TRUE(ov[i].t >= ov[i - 1].t);
    }
  }
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 25.0f, ov[0].T); // starts at ambient
}

void test_phase_boundaries_on_projected_timeline(void) {
  const OvenModel m = toyModel(); // heat 2 °C/s, cool 1 °C/s
  Phase phases[2] = {};
  phases[0].targetC = 125.0f; // 25→125 ASAP = 50 s projected ramp (100 °C / 2), + 60 s hold → 110 s
  phases[0].holdSeconds = 60.0f;
  phases[1].targetC = 25.0f; // 125→25 cool ASAP = 100 s projected (100 °C / 1), + 30 s hold → 130 s
  phases[1].holdSeconds = 30.0f;
  float bounds[kMaxPhases];
  const size_t n = profile_facts::samplePhaseBoundaries(phases, 2, RecipeMode::Reflow, m, 25.0f,
                                                        bounds, kMaxPhases);
  TEST_ASSERT_EQUAL_UINT(2, n); // ends at 25 °C (< touch-safe) so no implicit cool is appended
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 110.0f, bounds[0]); // 50 s projected ramp + 60 s hold
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 240.0f, bounds[1]); // + 100 s projected cool + 30 s hold
  TEST_ASSERT_TRUE(bounds[1] >= bounds[0]);          // monotonic
  // Boundaries sit exactly on the *projected* curve's phase corners (its last point == last bound).
  CurvePoint ach[kMaxCurvePoints];
  const size_t na = sampleCurve(phases, 2, RecipeMode::Reflow, m, /*achievable=*/true, 25.0f, ach,
                                kMaxCurvePoints);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, ach[na - 1].t, bounds[n - 1]);
}

void test_requested_curve_asap_ramp_is_vertical(void) {
  const OvenModel m = toyModel();
  Phase p = {};
  p.targetC = 125.0f; // 25→125, ASAP (rampSeconds 0)
  p.holdSeconds = 40.0f;
  CurvePoint req[kMaxCurvePoints];
  const size_t n =
      sampleCurve(&p, 1, RecipeMode::Reflow, m, /*achievable=*/false, 25.0f, req, kMaxCurvePoints);
  // The requested (authored) line jumps straight up: the ambient point and the target point share
  // t=0 (a vertical segment), rather than sloping over the achievable ramp time.
  TEST_ASSERT_TRUE(n >= 2);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, req[0].t);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 25.0f, req[0].T);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, req[1].t); // reaches target at t=0 (instant)
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 125.0f, req[1].T);
}

void test_phase_boundaries_use_projected_ramp_time(void) {
  const OvenModel m = toyModel(); // heat 2 °C/s
  // A timed ramp the oven cannot meet: 25→225 (projected 100 s at 2 °C/s) authored in 10 s. The
  // boundary lands on the PROJECTED 100 s (+ hold) — the plant-limited time the projected trace
  // actually needs to reach the setpoint — not the optimistic authored 10 s.
  Phase p = {};
  p.targetC = 225.0f;
  p.rampSeconds = 10.0f;
  p.holdSeconds = 20.0f;
  float bounds[profile_facts::kMaxCurvePhases];
  const size_t n = profile_facts::samplePhaseBoundaries(&p, 1, RecipeMode::Reflow, m, 25.0f, bounds,
                                                        profile_facts::kMaxCurvePhases);
  // Two boundaries: the authored phase, then the implicit passive cool-down (§6).
  TEST_ASSERT_EQUAL_UINT(2, n);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 120.0f, bounds[0]); // 100 s projected ramp + 20 s hold, not 30 s
  TEST_ASSERT_TRUE(bounds[1] > bounds[0]);           // cool-down extends past the reflow end
}

void test_phase_boundaries_asap_cool_has_projected_width(void) {
  const OvenModel m = toyModel(); // heat 2 °C/s, cool 1 °C/s
  // The LF-245 shape: a reflow peak, then an authored ASAP cool leg to 50 °C. On the projected
  // timeline that cool leg is NOT zero-width — it takes the real time to coast 245→50 (195 s at
  // 1 °C/s) — so its separator/label sits well clear of the reflow corner (the bug that squished
  // the two cool phases together on the old authored timeline).
  Phase phases[2] = {};
  phases[0].targetC = 245.0f;
  phases[0].rampSeconds = 40.0f; // projected 110 s (220 °C / 2), rate-limited from 40 s
  phases[0].holdSeconds = 30.0f;
  phases[1].targetC = 50.0f;    // 245→50 cool, ASAP
  phases[1].rampSeconds = 0.0f; // ASAP — the case that used to collapse to zero width
  float bounds[profile_facts::kMaxCurvePhases];
  const size_t n = profile_facts::samplePhaseBoundaries(phases, 2, RecipeMode::Reflow, m, 25.0f,
                                                        bounds, profile_facts::kMaxCurvePhases);
  // Reflow, the ASAP cool, then the implicit 50→43 safety cool (§6).
  TEST_ASSERT_EQUAL_UINT(3, n);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 140.0f, bounds[0]);             // 110 s projected ramp + 30 s hold
  TEST_ASSERT_FLOAT_WITHIN(2.0f, 195.0f, bounds[1] - bounds[0]); // the ASAP cool's real 195 s width
  TEST_ASSERT_TRUE(bounds[2] > bounds[1]); // implicit safety cool extends past it
}

void test_uv_spans_cover_uv_on_phases(void) {
  const OvenModel m = toyModel();
  Phase phases[3] = {};
  phases[0].targetC = 60.0f; // warm, no UV
  phases[1].targetC = 60.0f; // cure: UV on, hold 120 s
  phases[1].uv = true;
  phases[1].motor = true;
  phases[1].holdSeconds = 120.0f;
  phases[2].targetC = 30.0f; // cool, no UV
  profile_facts::TimeSpan spans[kMaxPhases];
  const size_t n =
      profile_facts::sampleUvSpans(phases, 3, RecipeMode::Cure, m, 25.0f, spans, kMaxPhases);
  TEST_ASSERT_EQUAL_UINT(1, n); // only the cure phase is UV-on
  TEST_ASSERT_TRUE(spans[0].end > spans[0].start);
  // Reflow carries no UV, so no spans regardless of content.
  TEST_ASSERT_EQUAL_UINT(
      0, profile_facts::sampleUvSpans(phases, 3, RecipeMode::Reflow, m, 25.0f, spans, kMaxPhases));
}

void test_overshoot_bounded_under_nonfinite_input(void) {
  const OvenModel m = toyModel();
  Phase phases[2] = {};
  phases[0].targetC = std::nanf("");
  phases[0].rampSeconds = INFINITY;
  phases[1].targetC = 1.0e30f;
  CurvePoint ov[kMaxCurvePoints];
  const size_t n = profile_facts::sampleOvershoot(phases, 2, RecipeMode::Reflow, m, std::nanf(""),
                                                  ov, kMaxCurvePoints);
  for (size_t i = 0; i < n; ++i) {
    TEST_ASSERT_TRUE(std::isfinite(ov[i].t) && std::isfinite(ov[i].T));
    TEST_ASSERT_TRUE(ov[i].T >= kTempLo && ov[i].T <= kTempHi);
  }
}

// --- formatters --------------------------------------------------------------------------------

void test_format_duration_minutes_and_hours(void) {
  char buf[24];
  formatDuration(370.0f, buf, sizeof(buf)); // 6 min 10 s
  TEST_ASSERT_EQUAL_STRING("~6:10", buf);
  formatDuration(3723.0f, buf, sizeof(buf)); // 1 h 2 m 3 s
  TEST_ASSERT_EQUAL_STRING("~1:02:03", buf);
  formatDuration(0.0f, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("~0:00", buf);
}

void test_format_duration_guards_nonfinite(void) {
  char buf[24];
  formatDuration(std::nanf(""), buf, sizeof(buf)); // must not crash / print garbage
  TEST_ASSERT_EQUAL_STRING("~0:00", buf);
}

void test_format_peak_celsius_and_fahrenheit(void) {
  char buf[24];
  formatPeak(245.0f, /*fahrenheit=*/false, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("peak 245\xC2\xB0", buf);
  formatPeak(100.0f, /*fahrenheit=*/true, buf, sizeof(buf)); // 100 °C = 212 °F
  TEST_ASSERT_EQUAL_STRING("peak 212\xC2\xB0", buf);
}

// --- implicit cool-down (§6) ------------------------------------------------------------------

void test_implicit_cool_appended_when_run_ends_hot(void) {
  const OvenModel m = toyModel();
  // A single hot phase (245 °C, no cool phase authored). The preview must append a passive
  // cool-down so the curve ends at the touch-safe temperature, and add one phase boundary + a
  // "Cool" run flag.
  Phase p = {};
  p.targetC = 245.0f;
  p.rampSeconds = 40.0f;
  p.holdSeconds = 30.0f;
  CurvePoint pts[kMaxCurvePoints];
  const size_t n =
      sampleCurve(&p, 1, RecipeMode::Reflow, m, /*achievable=*/false, 25.0f, pts, kMaxCurvePoints);
  TEST_ASSERT_TRUE(n >= 3);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, kTouchSafeC, pts[n - 1].T); // curve ends touch-safe
  TEST_ASSERT_TRUE(pts[n - 1].t > pts[n - 2].t);             // the cool-down took real time
  TEST_ASSERT_TRUE(profile_facts::runHasImplicitCool(&p, 1, m, 25.0f));
}

void test_no_implicit_cool_when_run_ends_cool(void) {
  const OvenModel m = toyModel();
  // A run that already ends at/below touch-safe needs no cool-down tail.
  Phase p = {};
  p.targetC = 40.0f; // below kTouchSafeC (43)
  p.rampSeconds = 30.0f;
  p.holdSeconds = 20.0f;
  TEST_ASSERT_FALSE(profile_facts::runHasImplicitCool(&p, 1, m, 25.0f));
  float bounds[profile_facts::kMaxCurvePhases];
  const size_t n = profile_facts::samplePhaseBoundaries(&p, 1, RecipeMode::Reflow, m, 25.0f, bounds,
                                                        profile_facts::kMaxCurvePhases);
  TEST_ASSERT_EQUAL_UINT(1, n); // just the authored phase — no appended cool boundary
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_facts_peak_is_hottest_setpoint);
  RUN_TEST(test_facts_duration_ramp_plus_hold);
  RUN_TEST(test_facts_empty_profile_is_zero);
  RUN_TEST(test_facts_cure_hold_from_exposure);
  RUN_TEST(test_curve_starts_at_ambient_and_is_monotonic_in_time);
  RUN_TEST(test_curve_requested_vs_achievable_diverge_when_rate_limited);
  RUN_TEST(test_curve_bounded_under_nonfinite_input);
  RUN_TEST(test_achievable_curve_resolves_fan_auto);
  RUN_TEST(test_any_ramp_rate_limited);
  RUN_TEST(test_overshoot_bounded_and_monotonic);
  RUN_TEST(test_phase_boundaries_on_projected_timeline);
  RUN_TEST(test_phase_boundaries_use_projected_ramp_time);
  RUN_TEST(test_phase_boundaries_asap_cool_has_projected_width);
  RUN_TEST(test_requested_curve_asap_ramp_is_vertical);
  RUN_TEST(test_uv_spans_cover_uv_on_phases);
  RUN_TEST(test_overshoot_bounded_under_nonfinite_input);
  RUN_TEST(test_format_duration_minutes_and_hours);
  RUN_TEST(test_format_duration_guards_nonfinite);
  RUN_TEST(test_format_peak_celsius_and_fahrenheit);
  RUN_TEST(test_implicit_cool_appended_when_run_ends_hot);
  RUN_TEST(test_no_implicit_cool_when_run_ends_cool);
  return UNITY_END();
}
