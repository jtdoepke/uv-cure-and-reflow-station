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
  // reaches target in 10 s; the achievable one takes the full 100 s — so the total spans differ.
  Phase p = {};
  p.targetC = 225.0f;
  p.rampSeconds = 10.0f;
  CurvePoint req[kMaxCurvePoints];
  CurvePoint ach[kMaxCurvePoints];
  const size_t nr = sampleCurve(&p, 1, RecipeMode::Reflow, m, false, 25.0f, req, kMaxCurvePoints);
  const size_t na = sampleCurve(&p, 1, RecipeMode::Reflow, m, true, 25.0f, ach, kMaxCurvePoints);
  TEST_ASSERT_TRUE(nr >= 2 && na >= 2);
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 10.0f, req[nr - 1].t);  // requested: honored 10 s
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 100.0f, ach[na - 1].t); // achievable: rate-limited to 100 s
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

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_facts_peak_is_hottest_setpoint);
  RUN_TEST(test_facts_duration_ramp_plus_hold);
  RUN_TEST(test_facts_empty_profile_is_zero);
  RUN_TEST(test_facts_cure_hold_from_exposure);
  RUN_TEST(test_curve_starts_at_ambient_and_is_monotonic_in_time);
  RUN_TEST(test_curve_requested_vs_achievable_diverge_when_rate_limited);
  RUN_TEST(test_curve_bounded_under_nonfinite_input);
  RUN_TEST(test_format_duration_minutes_and_hours);
  RUN_TEST(test_format_duration_guards_nonfinite);
  RUN_TEST(test_format_peak_celsius_and_fahrenheit);
  return UNITY_END();
}
