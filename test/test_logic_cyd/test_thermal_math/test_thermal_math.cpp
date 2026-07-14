// native_logic_cyd suite — pure host tests of the shared rate-limit / first-order-lag math
// (thermal_math.h) and the stub calibration defaults (oven_cal.h). No LVGL, no Arduino: plain
// plant maths — ramp-duration integral, feasibility clamp, low-pass lag, exposure↔hold,
// feedforward duty, and a sanity check on oven_cal::DEFAULT. Backlog B2 (design.md §5/§6/§12/§15).
#include <unity.h>

#include "oven_cal.h"
#include "thermal_math.h"

void setUp(void) {}
void tearDown(void) {}

// A tiny helper: assert two floats are within tol.
static void assert_close(float expected, float actual, float tol) {
  TEST_ASSERT_FLOAT_WITHIN(tol, expected, actual);
}

// --- Ramp duration (∫ dT/rate) -----------------------------------------------------------------

void test_ramp_duration_constant_rate(void) {
  // Constant 2 °C/s envelope → duration is exactly ΔT / rate, both directions.
  RateEnvelope env{0.0f, 2.0f, 0.05f, 2.0f};
  assert_close(50.0f, rampDurationSeconds(env, 20.0f, 120.0f), 1e-3f); // heating 100 °C
  assert_close(50.0f, rampDurationSeconds(env, 120.0f, 20.0f), 1e-3f); // cooling, same span
  assert_close(0.0f, rampDurationSeconds(env, 80.0f, 80.0f), 1e-6f);   // zero-width
}

void test_ramp_duration_affine_bounded_by_endpoints(void) {
  // Rate rises with T: rate(T) = 0.01·T + 0.5, over 0→100 °C. Faster at the hot end, so the true
  // time sits strictly between the two constant-rate extremes (slowest-rate and fastest-rate).
  RateEnvelope env{0.01f, 0.5f, 0.05f, 100.0f};
  const float slowRate = env.rate(0.0f);   // 0.5 °C/s
  const float fastRate = env.rate(100.0f); // 1.5 °C/s
  const float span = 100.0f;
  const float t = rampDurationSeconds(env, 0.0f, 100.0f);
  TEST_ASSERT_TRUE(t < span / slowRate); // faster than if it were slow the whole way
  TEST_ASSERT_TRUE(t > span / fastRate); // slower than if it were fast the whole way
}

void test_ramp_duration_respects_floor(void) {
  // A degenerate slope that would drive rate to 0/negative is floored, so the integral stays
  // finite rather than blowing up.
  RateEnvelope env{-1.0f, 0.0f, 0.05f, 1.0f}; // rate(T) would go very negative; floored to 0.05
  const float t = rampDurationSeconds(env, 0.0f, 10.0f);
  TEST_ASSERT_TRUE(t > 0.0f);
  assert_close(10.0f / 0.05f, t, 1.0f); // ~200 s at the floor
}

// --- Feasibility / rate limiting ---------------------------------------------------------------

void test_rate_limit_clamps_too_fast(void) {
  RateEnvelope env{0.0f, 1.0f, 0.05f, 1.0f};                    // 1 °C/s → 100 °C takes 100 s
  RampFeasibility r = rateLimitRamp(env, 20.0f, 120.0f, 40.0f); // asked for 40 s, impossible
  TEST_ASSERT_TRUE(r.rateLimited);
  assert_close(100.0f, r.achievableSeconds, 1e-3f);
}

void test_rate_limit_honors_slow_request(void) {
  RateEnvelope env{0.0f, 1.0f, 0.05f, 1.0f};
  RampFeasibility r = rateLimitRamp(env, 20.0f, 120.0f, 200.0f); // slower than achievable
  TEST_ASSERT_FALSE(r.rateLimited);
  assert_close(200.0f, r.achievableSeconds, 1e-3f);
}

void test_rate_limit_asap_returns_achievable(void) {
  RateEnvelope env{0.0f, 1.0f, 0.05f, 1.0f};
  RampFeasibility r = rateLimitRamp(env, 20.0f, 120.0f, 0.0f); // 0 = as-fast-as-possible
  TEST_ASSERT_FALSE(r.rateLimited);                            // ASAP is not a violation
  assert_close(100.0f, r.achievableSeconds, 1e-3f);
}

// --- First-order lag ---------------------------------------------------------------------------

void test_lp_step_converges_toward_input(void) {
  // Repeated steps toward a fixed input approach it monotonically and get close.
  float y = 0.0f;
  for (int i = 0; i < 2000; ++i)
    y = lpStep(y, 100.0f, 1.0f, 30.0f);
  assert_close(100.0f, y, 0.1f);
}

void test_lp_step_zero_tau_is_passthrough(void) {
  assert_close(100.0f, lpStep(0.0f, 100.0f, 1.0f, 0.0f), 1e-6f);
}

void test_board_temp_estimate_affine(void) {
  LagParams lag{1.5f, -4.0f, 30.0f};
  assert_close(1.5f * 80.0f - 4.0f, boardTempEstimate(lag, 80.0f), 1e-4f);
}

// --- Exposure ↔ hold seconds -------------------------------------------------------------------

void test_exposure_to_hold_and_roundtrip(void) {
  const float exposure = 30.0f, coverage = 0.25f;
  const float hold = exposureToHoldSeconds(exposure, coverage);
  assert_close(120.0f, hold, 1e-4f); // 30 / 0.25
  assert_close(exposure, perSurfaceExposure(hold, coverage), 1e-4f);
}

void test_exposure_guards_nonpositive_coverage(void) {
  TEST_ASSERT_EQUAL_FLOAT(0.0f, exposureToHoldSeconds(30.0f, 0.0f));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, exposureToHoldSeconds(30.0f, -0.1f));
}

// --- Fan-conditioned lookups -------------------------------------------------------------------

void test_fan_pick_selects_variant(void) {
  const OvenModel &m = oven_cal::DEFAULT;
  // Fan on heats & cools faster than fan off in the defaults.
  TEST_ASSERT_TRUE(heatRate(m, 100.0f, true) > heatRate(m, 100.0f, false));
  TEST_ASSERT_TRUE(coolRate(m, 100.0f, true) > coolRate(m, 100.0f, false));
}

// --- Feedforward duty --------------------------------------------------------------------------

void test_steady_state_duty_clamped(void) {
  // A duty model that would exceed 1 at high T is clamped into 0..1.
  OvenModel m = oven_cal::DEFAULT;
  m.duty.on = DutyModel{0.01f, 0.5f, 1.0f}; // 0.5 + 0.01·T → >1 above 50 °C
  const float d = steadyStateDuty(m, 300.0f, true);
  TEST_ASSERT_TRUE(d <= 1.0f && d >= 0.0f);
  assert_close(1.0f, d, 1e-6f);
}

// --- oven_cal::DEFAULT sanity ------------------------------------------------------------------

void test_default_model_is_uncalibrated_and_sane(void) {
  const OvenModel &m = oven_cal::DEFAULT;
  TEST_ASSERT_FALSE(m.calibrated);
  TEST_ASSERT_TRUE(m.beamCoverage > 0.0f && m.beamCoverage <= 1.0f);
  TEST_ASSERT_TRUE(m.turntableRpm > 0.0f);
  TEST_ASSERT_TRUE(heatRate(m, 150.0f, false) > 0.0f);
  TEST_ASSERT_TRUE(coolRate(m, 150.0f, false) > 0.0f);
  TEST_ASSERT_TRUE(m.lag.off.tau > 0.0f && m.lag.on.tau > 0.0f);
}

void test_default_model_gives_idealized_linear_duration(void) {
  // Uncalibrated (slope-0) envelopes → duration is exactly ΔT / constant-rate.
  const OvenModel &m = oven_cal::DEFAULT;
  const float rate = heatRate(m, 100.0f, false); // constant across T
  const float t = rampDurationSeconds(m.heat.off, 20.0f, 120.0f);
  assert_close(100.0f / rate, t, 1e-2f);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_ramp_duration_constant_rate);
  RUN_TEST(test_ramp_duration_affine_bounded_by_endpoints);
  RUN_TEST(test_ramp_duration_respects_floor);
  RUN_TEST(test_rate_limit_clamps_too_fast);
  RUN_TEST(test_rate_limit_honors_slow_request);
  RUN_TEST(test_rate_limit_asap_returns_achievable);
  RUN_TEST(test_lp_step_converges_toward_input);
  RUN_TEST(test_lp_step_zero_tau_is_passthrough);
  RUN_TEST(test_board_temp_estimate_affine);
  RUN_TEST(test_exposure_to_hold_and_roundtrip);
  RUN_TEST(test_exposure_guards_nonpositive_coverage);
  RUN_TEST(test_fan_pick_selects_variant);
  RUN_TEST(test_steady_state_duty_clamped);
  RUN_TEST(test_default_model_is_uncalibrated_and_sane);
  RUN_TEST(test_default_model_gives_idealized_linear_duration);
  return UNITY_END();
}
