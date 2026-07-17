// native_logic_cyd suite — the phase→segment recipe compiler (recipe_compiler.h; backlog B1,
// design.md §5/§12). Covers ramp interp selection, the cure exposure→hold conversion + its
// fallback/estimate labeling, the amber (rate-limited / heuristic / uncalibrated) flags, and the
// hard-reject tier. A cross-check asserts every hard-valid compile satisfies the invariants the
// controller's RecipeValidator (A7) enforces, so an accepted recipe never NAKs on upload.
#include <cmath>

#include <unity.h>

#include "oven_cal.h"
#include "recipe_compiler.h"

void setUp(void) {}
void tearDown(void) {}

namespace {

constexpr float kAmbient = 22.0f;
const Caps kReflowCaps{0.0f, 300.0f};
const Caps kCureCaps{0.0f, 120.0f};

RateEnvelope constRate(float r) {
  return RateEnvelope{0.0f, r, 0.01f, r};
}

// Toy calibrated plant: fan-off 1 °C/s heat / 0.5 °C/s cool, fan-on twice that; beamCoverage 0.5.
OvenModel calibratedModel() {
  OvenModel m{};
  m.heat = FanPair<RateEnvelope>{constRate(1.0f), constRate(2.0f)};
  m.cool = FanPair<RateEnvelope>{constRate(0.5f), constRate(1.0f)};
  m.lag = FanPair<LagParams>{LagParams{1.0f, 0.0f, 10.0f}, LagParams{1.0f, 0.0f, 10.0f}};
  m.duty = FanPair<DutyModel>{DutyModel{0.0f, 0.5f, 1.0f}, DutyModel{0.0f, 0.5f, 1.0f}};
  m.beamCoverage = 0.5f;
  m.turntableRpm = 30.0f;
  m.calibrated = true;
  return m;
}

// Reproduce (without linking) the controller-side RecipeValidator (A7) checks: >=1 segment, every
// dur_ms > 0 and heat_c finite, within [0, content-derived hard-max], and a reflow tag asserts no
// uv/motor. Mirrors oven_safety.h (CURE_HARD_MAX_C = 120, REFLOW_HARD_MAX_C = 300).
void assertUploadable(const oven_Recipe &rec) {
  TEST_ASSERT_TRUE(rec.segments_count >= 1);
  bool hasUv = false, hasMotor = false;
  float maxHeat = rec.segments[0].heat_c, minHeat = rec.segments[0].heat_c;
  for (pb_size_t i = 0; i < rec.segments_count; ++i) {
    const oven_Segment &s = rec.segments[i];
    TEST_ASSERT_TRUE(s.dur_ms > 0);
    TEST_ASSERT_TRUE(std::isfinite(s.heat_c));
    hasUv = hasUv || s.uv;
    hasMotor = hasMotor || s.motor;
    maxHeat = s.heat_c > maxHeat ? s.heat_c : maxHeat;
    minHeat = s.heat_c < minHeat ? s.heat_c : minHeat;
  }
  if (rec.mode == oven_Mode_MODE_REFLOW) {
    TEST_ASSERT_FALSE(hasUv);
    TEST_ASSERT_FALSE(hasMotor);
  }
  const float cap = (hasUv || hasMotor) ? 120.0f : 300.0f;
  TEST_ASSERT_TRUE(maxHeat <= cap);
  TEST_ASSERT_TRUE(minHeat >= 0.0f);
}

} // namespace

void test_reflow_ramp_over_time_then_hold(void) {
  const OvenModel m = calibratedModel();
  Phase phases[2];
  phases[0].targetC = 100.0f;
  phases[0].rampSeconds = 80.0f; // 78 °C over 80 s → 0.975 °C/s, fan-off (1 °C/s) covers it
  phases[0].holdSeconds = 60.0f;
  phases[1].targetC = 150.0f;
  phases[1].rampSeconds = 0.0f; // ASAP
  phases[1].holdSeconds = 30.0f;

  CompileResult r = compileRecipe(phases, 2, RecipeMode::Reflow, m, kReflowCaps, kAmbient, 7, 3);

  TEST_ASSERT_TRUE(r.hardValid);
  TEST_ASSERT_EQUAL(oven_Mode_MODE_REFLOW, r.recipe.mode);
  TEST_ASSERT_EQUAL_UINT32(7, r.recipe.id);
  TEST_ASSERT_EQUAL_UINT32(3, r.recipe.seq);
  TEST_ASSERT_EQUAL(4, r.recipe.segments_count);

  TEST_ASSERT_EQUAL(oven_Interp_INTERP_RAMP_OVER_TIME, r.recipe.segments[0].interp);
  TEST_ASSERT_EQUAL_UINT32(80000, r.recipe.segments[0].dur_ms);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 100.0f, r.recipe.segments[0].heat_c);
  TEST_ASSERT_FALSE(r.recipe.segments[0].conv_fan); // meetable without the fan

  TEST_ASSERT_EQUAL(oven_Interp_INTERP_HOLD, r.recipe.segments[1].interp);
  TEST_ASSERT_EQUAL_UINT32(60000, r.recipe.segments[1].dur_ms);

  TEST_ASSERT_EQUAL(oven_Interp_INTERP_RAMP_ASAP, r.recipe.segments[2].interp);
  TEST_ASSERT_EQUAL_UINT32(25000, r.recipe.segments[2].dur_ms); // 50 °C at fan-on 2 °C/s = 25 s
  TEST_ASSERT_TRUE(r.recipe.segments[2].conv_fan);              // ASAP picks the faster envelope

  TEST_ASSERT_EQUAL(oven_Interp_INTERP_HOLD, r.recipe.segments[3].interp);
  TEST_ASSERT_EQUAL_UINT32(30000, r.recipe.segments[3].dur_ms);

  TEST_ASSERT_FALSE(r.hasAmber()); // calibrated, nothing rate-limited
  assertUploadable(r.recipe);
}

void test_cure_exposure_to_hold_calibrated(void) {
  const OvenModel m = calibratedModel(); // beamCoverage 0.5
  Phase p;
  p.targetC = 80.0f;
  p.rampSeconds = 0.0f;
  p.uv = true;
  p.motor = true;
  p.exposurePerSurface = 30.0f; // → hold 30 / 0.5 = 60 s

  CompileResult r = compileRecipe(&p, 1, RecipeMode::Cure, m, kCureCaps, kAmbient, 1, 1);

  TEST_ASSERT_TRUE(r.hardValid);
  TEST_ASSERT_EQUAL(oven_Mode_MODE_CURE, r.recipe.mode);
  TEST_ASSERT_EQUAL(2, r.recipe.segments_count);
  TEST_ASSERT_EQUAL(oven_Interp_INTERP_HOLD, r.recipe.segments[1].interp);
  TEST_ASSERT_EQUAL_UINT32(60000, r.recipe.segments[1].dur_ms);
  TEST_ASSERT_TRUE(r.recipe.segments[1].uv);
  TEST_ASSERT_TRUE(r.recipe.segments[1].motor);
  TEST_ASSERT_FALSE(r.phases[0].holdEstimated); // calibrated → not an estimate
  TEST_ASSERT_FALSE(r.phases[0].holdFallback);
  assertUploadable(r.recipe);
}

void test_cure_exposure_uncalibrated_is_estimated(void) {
  const OvenModel &m = oven_cal::kDefaultModel; // calibrated == false, beamCoverage 0.25
  Phase p;
  p.targetC = 80.0f;
  p.rampSeconds = 0.0f;
  p.uv = true;
  p.motor = true;
  p.exposurePerSurface = 30.0f; // → hold 30 / 0.25 = 120 s (using the placeholder coverage)

  CompileResult r = compileRecipe(&p, 1, RecipeMode::Cure, m, kCureCaps, kAmbient, 1, 1);

  TEST_ASSERT_TRUE(r.hardValid);
  TEST_ASSERT_EQUAL(oven_Interp_INTERP_HOLD, r.recipe.segments[1].interp);
  TEST_ASSERT_EQUAL_UINT32(120000, r.recipe.segments[1].dur_ms);
  TEST_ASSERT_TRUE(r.phases[0].holdEstimated); // computed from a placeholder coverage
  TEST_ASSERT_TRUE(r.uncalibratedPreview);     // whole preview idealized
  TEST_ASSERT_TRUE(r.phases[0].fanHeuristic);  // Auto fan resolved by heuristic
  TEST_ASSERT_TRUE(r.hasAmber());
  assertUploadable(r.recipe);
}

void test_cure_turntable_off_falls_back_to_seconds(void) {
  const OvenModel m = calibratedModel();
  Phase p;
  p.targetC = 80.0f;
  p.rampSeconds = 0.0f;
  p.uv = true;
  p.motor = false; // turntable off → no even exposure → raw seconds
  p.exposurePerSurface = 30.0f;
  p.holdSeconds = 45.0f;

  CompileResult r = compileRecipe(&p, 1, RecipeMode::Cure, m, kCureCaps, kAmbient, 1, 1);

  TEST_ASSERT_TRUE(r.hardValid);
  TEST_ASSERT_EQUAL(oven_Interp_INTERP_HOLD, r.recipe.segments[1].interp);
  TEST_ASSERT_EQUAL_UINT32(45000, r.recipe.segments[1].dur_ms); // raw holdSeconds, not the dose
  TEST_ASSERT_FALSE(r.phases[0].holdEstimated);
  TEST_ASSERT_TRUE(r.phases[0].holdFallback);
  assertUploadable(r.recipe);
}

void test_amber_rate_limited_still_uploadable(void) {
  const OvenModel m = calibratedModel();
  Phase p;
  p.targetC = 120.0f;
  p.rampSeconds = 40.0f; // 100 °C in 40 s = 2.5 °C/s, faster than even fan-on (2 °C/s)

  CompileResult r = compileRecipe(&p, 1, RecipeMode::Reflow, m, kReflowCaps, 20.0f, 1, 1);

  TEST_ASSERT_TRUE(r.hardValid); // physically-optimistic, but still saveable/uploadable (§12)
  TEST_ASSERT_EQUAL(1, r.recipe.segments_count);
  TEST_ASSERT_EQUAL(oven_Interp_INTERP_RAMP_OVER_TIME, r.recipe.segments[0].interp);
  TEST_ASSERT_EQUAL_UINT32(40000,
                           r.recipe.segments[0].dur_ms); // requested sweep kept, not rewritten
  TEST_ASSERT_TRUE(r.recipe.segments[0].conv_fan);       // Auto turned the fan on to try
  TEST_ASSERT_TRUE(r.phases[0].rampRateLimited);
  TEST_ASSERT_TRUE(r.hasAmber());
  assertUploadable(r.recipe);
}

void test_omit_degenerate_ramp(void) {
  const OvenModel m = calibratedModel();
  Phase p;
  p.targetC = kAmbient; // no temperature change from the start → no ramp segment
  p.holdSeconds = 10.0f;

  CompileResult r = compileRecipe(&p, 1, RecipeMode::Reflow, m, kReflowCaps, kAmbient, 1, 1);

  TEST_ASSERT_TRUE(r.hardValid);
  TEST_ASSERT_EQUAL(1, r.recipe.segments_count);
  TEST_ASSERT_EQUAL(oven_Interp_INTERP_HOLD, r.recipe.segments[0].interp);
}

void test_reject_no_phases(void) {
  const OvenModel m = calibratedModel();
  CompileResult r = compileRecipe(nullptr, 0, RecipeMode::Reflow, m, kReflowCaps, kAmbient, 1, 1);
  TEST_ASSERT_FALSE(r.hardValid);
  TEST_ASSERT_EQUAL(CompileReject::NoPhases, r.reject);
}

void test_reject_target_over_cap(void) {
  const OvenModel m = calibratedModel();
  Phase p;
  p.targetC = 350.0f; // above the 300 reflow cap
  p.holdSeconds = 10.0f;
  CompileResult r = compileRecipe(&p, 1, RecipeMode::Reflow, m, kReflowCaps, kAmbient, 1, 1);
  TEST_ASSERT_FALSE(r.hardValid);
  TEST_ASSERT_EQUAL(CompileReject::TargetOutOfRange, r.reject);
  TEST_ASSERT_EQUAL(0, r.rejectPhase);
  TEST_ASSERT_EQUAL(0, r.recipe.segments_count);
}

void test_reject_nonfinite_target(void) {
  const OvenModel m = calibratedModel();
  Phase p;
  p.targetC = NAN;
  p.holdSeconds = 10.0f;
  CompileResult r = compileRecipe(&p, 1, RecipeMode::Reflow, m, kReflowCaps, kAmbient, 1, 1);
  TEST_ASSERT_FALSE(r.hardValid);
  TEST_ASSERT_EQUAL(CompileReject::NonFiniteTarget, r.reject);
}

void test_reject_reflow_with_uv(void) {
  const OvenModel m = calibratedModel();
  Phase p;
  p.targetC = 100.0f;
  p.holdSeconds = 10.0f;
  p.uv = true; // a reflow recipe must not assert uv/motor
  CompileResult r = compileRecipe(&p, 1, RecipeMode::Reflow, m, kReflowCaps, kAmbient, 1, 1);
  TEST_ASSERT_FALSE(r.hardValid);
  TEST_ASSERT_EQUAL(CompileReject::ModeContentMismatch, r.reject);
}

void test_reject_too_many_segments(void) {
  const OvenModel m = calibratedModel();
  Phase phases[40];
  for (auto &p : phases) {
    p.targetC = 100.0f; // same target → only the first phase ramps; the rest are hold-only
    p.holdSeconds = 5.0f;
  }
  CompileResult r = compileRecipe(phases, 40, RecipeMode::Reflow, m, kReflowCaps, kAmbient, 1, 1);
  TEST_ASSERT_FALSE(r.hardValid);
  TEST_ASSERT_EQUAL(CompileReject::TooManySegments, r.reject);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_reflow_ramp_over_time_then_hold);
  RUN_TEST(test_cure_exposure_to_hold_calibrated);
  RUN_TEST(test_cure_exposure_uncalibrated_is_estimated);
  RUN_TEST(test_cure_turntable_off_falls_back_to_seconds);
  RUN_TEST(test_amber_rate_limited_still_uploadable);
  RUN_TEST(test_omit_degenerate_ramp);
  RUN_TEST(test_reject_no_phases);
  RUN_TEST(test_reject_target_over_cap);
  RUN_TEST(test_reject_nonfinite_target);
  RUN_TEST(test_reject_reflow_with_uv);
  RUN_TEST(test_reject_too_many_segments);
  return UNITY_END();
}
