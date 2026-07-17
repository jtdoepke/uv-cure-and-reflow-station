// native_logic_cyd suite — the fan-`Auto` resolver (fan_resolver.h; backlog B3, design.md §5).
// Explicit On/Off pass through; `Auto` consults the fan-conditioned envelopes when calibrated and
// falls back to the "on while heating / on while cooling" heuristic (flagged) before calibration.
#include <unity.h>

#include "fan_resolver.h"
#include "oven_cal.h"

void setUp(void) {}
void tearDown(void) {}

namespace {

RateEnvelope constRate(float r) {
  return RateEnvelope{0.0f, r, 0.01f, r};
}

// A toy *calibrated* plant: fan-on heats/cools twice as fast as fan-off, so Auto has a real
// on-vs-off decision to make.
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

} // namespace

void test_explicit_on_off_passthrough(void) {
  const OvenModel m = calibratedModel();
  FanDecision d = resolveFans(FanMode::On, FanMode::Off, FanContext{20.0f, 200.0f, 0.0f}, m);
  TEST_ASSERT_TRUE(d.convFan);
  TEST_ASSERT_FALSE(d.coolFan);
  TEST_ASSERT_FALSE(d.heuristic); // explicit choices are not heuristic
}

void test_uncalibrated_heuristic_on_while_heating(void) {
  const OvenModel &m = oven_cal::kDefaultModel; // calibrated == false
  FanDecision heat = resolveFans(FanMode::Auto, FanMode::Auto, FanContext{20.0f, 120.0f, 0.0f}, m);
  TEST_ASSERT_TRUE(heat.convFan);   // heating → conv on
  TEST_ASSERT_FALSE(heat.coolFan);  // not cooling → cool off
  TEST_ASSERT_TRUE(heat.heuristic); // resolved without real envelopes → flagged
}

void test_uncalibrated_heuristic_on_while_cooling(void) {
  const OvenModel &m = oven_cal::kDefaultModel;
  FanDecision cool = resolveFans(FanMode::Auto, FanMode::Auto, FanContext{120.0f, 30.0f, 0.0f}, m);
  TEST_ASSERT_FALSE(cool.convFan);
  TEST_ASSERT_TRUE(cool.coolFan);
  TEST_ASSERT_TRUE(cool.heuristic);
}

void test_calibrated_conv_off_when_ramp_meetable(void) {
  const OvenModel m = calibratedModel(); // fan-off heats at 1 °C/s
  // 100 °C over 200 s needs 0.5 °C/s — the fan-off envelope covers it, so Auto leaves conv off.
  FanDecision d = resolveFans(FanMode::Auto, FanMode::Off, FanContext{20.0f, 120.0f, 200.0f}, m);
  TEST_ASSERT_FALSE(d.convFan);
  TEST_ASSERT_FALSE(d.heuristic); // calibrated path
}

void test_calibrated_conv_on_when_ramp_too_fast(void) {
  const OvenModel m = calibratedModel();
  // 100 °C over 40 s needs 2.5 °C/s — fan-off (1 °C/s) can't, so Auto turns conv on.
  FanDecision d = resolveFans(FanMode::Auto, FanMode::Off, FanContext{20.0f, 120.0f, 40.0f}, m);
  TEST_ASSERT_TRUE(d.convFan);
}

void test_calibrated_asap_picks_faster_envelope(void) {
  const OvenModel m = calibratedModel();
  // ASAP heating: fan-on reaches target sooner → conv on. ASAP cooling: cool-on sooner → cool on.
  FanDecision heat = resolveFans(FanMode::Auto, FanMode::Auto, FanContext{20.0f, 120.0f, 0.0f}, m);
  TEST_ASSERT_TRUE(heat.convFan);
  FanDecision cool = resolveFans(FanMode::Auto, FanMode::Auto, FanContext{120.0f, 20.0f, 0.0f}, m);
  TEST_ASSERT_TRUE(cool.coolFan);
}

void test_calibrated_cool_off_when_not_cooling(void) {
  const OvenModel m = calibratedModel();
  FanDecision d = resolveFans(FanMode::Off, FanMode::Auto, FanContext{20.0f, 120.0f, 0.0f}, m);
  TEST_ASSERT_FALSE(d.coolFan); // heating, so cooling fan stays off
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_explicit_on_off_passthrough);
  RUN_TEST(test_uncalibrated_heuristic_on_while_heating);
  RUN_TEST(test_uncalibrated_heuristic_on_while_cooling);
  RUN_TEST(test_calibrated_conv_off_when_ramp_meetable);
  RUN_TEST(test_calibrated_conv_on_when_ramp_too_fast);
  RUN_TEST(test_calibrated_asap_picks_faster_envelope);
  RUN_TEST(test_calibrated_cool_off_when_not_cooling);
  return UNITY_END();
}
