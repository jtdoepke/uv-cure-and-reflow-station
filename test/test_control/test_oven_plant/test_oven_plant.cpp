// native_control suite — the A10 thermal-plant twin (lib/plant/oven_plant.h).
//
// Asserts the physical behaviours that make the sim a faithful stand-in for the donor oven and that
// distinguish an energy model from the old flat-rate placeholder: a loss-limited temperature
// ceiling, asymptotic cooling toward ambient, the convection fan speeding the ramp, element-mass
// overshoot after shutoff, the workpiece lagging the chamber, the bay warming, and — above all —
// finite/bounded outputs under adversarial input (the fuzz invariant, pinned here too).
#include <cmath>
#include <initializer_list>
#include <limits>
#include <unity.h>

#include "oven_cal.h" // the linearized planning model — must stay consistent with the plant
#include "oven_plant.h"

namespace {

// Run the plant for `seconds` at fixed inputs, stepping every `dt` s. Returns nothing; inspect the
// plant afterwards.
void run(OvenPlant &p, float seconds, float dt, float duty, bool fan, bool uv = false) {
  for (float t = 0.0f; t < seconds; t += dt) {
    p.step(dt, duty, fan, uv, false);
  }
}

const float kAmbient = 25.0f;

// Full-duty (fan-off) chamber heat rate measured AT temperature T: warm to T, then average dT/dt.
float measuredHeatRate(float T) {
  OvenPlant p;
  while (p.chamberTempC() < T) {
    p.step(1.0f, 1.0f, false, false, false);
  }
  const float a = p.chamberTempC();
  for (int i = 0; i < 20; ++i) {
    p.step(1.0f, 1.0f, false, false, false);
  }
  return (p.chamberTempC() - a) / 20.0f;
}

// Passive (duty 0) chamber cool rate measured AT temperature T: heat above T, coast down to T, then
// average −dT/dt.
float measuredCoolRate(float T) {
  OvenPlant p;
  while (p.chamberTempC() < T + 40.0f) {
    p.step(1.0f, 1.0f, false, false, false);
  }
  while (p.chamberTempC() > T) {
    p.step(1.0f, 0.0f, false, false, false);
  }
  const float a = p.chamberTempC();
  for (int i = 0; i < 20; ++i) {
    p.step(1.0f, 0.0f, false, false, false);
  }
  return (a - p.chamberTempC()) / 20.0f;
}

} // namespace

void setUp(void) {}
void tearDown(void) {}

// Full duty forever settles at the loss-limited ceiling (≈ ambient + P/UA), never runs away. The
// old flat-rate model would ramp past any bound — this is the behaviour that required the twin.
void test_heating_reaches_a_loss_limited_ceiling(void) {
  OvenPlant p; // defaults: P=1500, C=2000, UA=5.9 → ceiling ≈ 25 + 254 ≈ 279 °C
  run(p, /*seconds=*/4.0f * 3600.0f, /*dt=*/1.0f, /*duty=*/1.0f, /*fan=*/false);
  const float c = p.chamberTempC();
  TEST_ASSERT_TRUE(std::isfinite(c));
  TEST_ASSERT_TRUE(c > 240.0f); // genuinely hot — can reach reflow range, with difficulty
  TEST_ASSERT_TRUE(c <
                   300.0f); // but bounded by losses (ceiling ≈ Tamb + P/UA ≈ 279 °C), not unbounded

  // Near the ceiling the heating rate has collapsed — one more hour barely moves it.
  const float before = p.chamberTempC();
  run(p, 3600.0f, 1.0f, 1.0f, false);
  TEST_ASSERT_TRUE(p.chamberTempC() - before < 5.0f);
}

// Heater off from hot: cooling slows as it approaches ambient (asymptotic), not a flat rate, and it
// never undershoots ambient. Replaces the old unphysical flat 0.2 °C/s.
void test_cooling_asymptotes_to_ambient(void) {
  OvenPlant p;
  run(p, 3600.0f, 1.0f, 1.0f, true); // heat up first
  TEST_ASSERT_TRUE(p.chamberTempC() > 150.0f);

  run(p, 3.0f * 3600.0f, 1.0f, /*duty=*/0.0f, /*fan=*/false); // long passive coast
  const float c = p.chamberTempC();
  TEST_ASSERT_TRUE(c < kAmbient + 8.0f); // settled near ambient
  TEST_ASSERT_TRUE(c > kAmbient - 2.0f); // never below ambient (one-sided heater, passive cooling)
}

// The convection fan delivers element heat faster, so the chamber ramps quicker for the same duty.
void test_fan_on_ramps_faster(void) {
  OvenPlant a, b;
  run(a, 300.0f, 1.0f, 1.0f, /*fan=*/false);
  run(b, 300.0f, 1.0f, 1.0f, /*fan=*/true);
  TEST_ASSERT_TRUE(b.chamberTempC() > a.chamberTempC());
}

// The heavy element keeps delivering heat after the duty is cut — the chamber overshoots for a
// while before it turns over. This is the post-shutoff overshoot the design's feedforward tames.
void test_element_mass_causes_overshoot_after_cutoff(void) {
  OvenPlant p;
  run(p, 600.0f, 1.0f, 1.0f, false); // drive the element hot
  const float atCut = p.chamberTempC();
  TEST_ASSERT_TRUE(p.elementTempC() > atCut); // element hotter than chamber at the cut

  // After cutting duty the chamber keeps rising for a while (element still dumping heat) before it
  // turns over — track the peak of that coast, which must exceed the value at the cut.
  float peak = atCut;
  for (int s = 0; s < 60; ++s) {
    p.step(1.0f, 0.0f, false, false, false);
    if (p.chamberTempC() > peak) {
      peak = p.chamberTempC();
    }
  }
  TEST_ASSERT_TRUE(peak > atCut + 0.5f); // a real post-shutoff overshoot occurred
}

// The workpiece is a first-order lag of the chamber: it trails during a ramp and converges at hold.
void test_workpiece_lags_chamber(void) {
  OvenPlant p;
  run(p, 120.0f, 1.0f, 1.0f, false);                       // mid-ramp
  TEST_ASSERT_TRUE(p.workpieceTempC() < p.chamberTempC()); // trailing

  run(p, 2.0f * 3600.0f, 1.0f, 1.0f, false);                            // long hold near ceiling
  TEST_ASSERT_FLOAT_WITHIN(5.0f, p.chamberTempC(), p.workpieceTempC()); // converged
}

// The always-on cooling fan's bay warms during a hot run (what the §6 ambient sensor reads), but
// stays cooler than the chamber it is shedding heat from.
void test_bay_warms_but_stays_below_chamber(void) {
  OvenPlant p;
  run(p, 3600.0f, 1.0f, 1.0f, true);
  TEST_ASSERT_TRUE(p.bayTempC() > kAmbient + 10.0f); // warmed
  TEST_ASSERT_TRUE(p.bayTempC() < p.chamberTempC()); // but below the chamber
}

// Adversarial input never produces a non-finite or out-of-band reading (the fuzz invariant,
// pinned).
void test_bounded_under_adversarial_input(void) {
  OvenPlant p;
  const float nan = std::nanf("");
  const float inf = std::numeric_limits<float>::infinity();
  p.step(nan, 1.0f, true, false, false);    // NaN dt
  p.step(inf, 1.0f, true, false, false);    // Inf dt
  p.step(-5.0f, 1.0f, true, false, false);  // negative dt
  p.step(1.0f, nan, true, false, false);    // NaN duty
  p.step(1.0f, inf, false, true, true);     // Inf duty
  p.step(1.0e30f, 5.0f, true, true, false); // absurd dt (clamped internally)
  TEST_ASSERT_TRUE(std::isfinite(p.chamberTempC()));
  TEST_ASSERT_TRUE(std::isfinite(p.workpieceTempC()));
  TEST_ASSERT_TRUE(std::isfinite(p.wallTempC()));
  TEST_ASSERT_TRUE(std::isfinite(p.bayTempC()));
  TEST_ASSERT_TRUE(std::isfinite(p.elementTempC()));
  TEST_ASSERT_TRUE(p.chamberTempC() > -100.1f && p.chamberTempC() < 5000.1f);
}

// --- Cal-consistency: oven_cal.h is the linearization of THIS plant, so the CYD preview and the
// sim agree (the A10 requirement). These pin the two together so neither drifts silently. ---

// The heat envelope tracks the plant's full-duty ramp rate across the range.
void test_cal_heat_envelope_matches_plant(void) {
  for (float T : {50.0f, 100.0f, 150.0f, 200.0f}) {
    TEST_ASSERT_FLOAT_WITHIN(0.06f, measuredHeatRate(T), oven_cal::HEAT_FAN_OFF.rate(T));
  }
}

// The cool envelope tracks the plant's passive cooldown rate.
void test_cal_cool_envelope_matches_plant(void) {
  for (float T : {150.0f, 100.0f, 60.0f}) {
    TEST_ASSERT_FLOAT_WITHIN(0.05f, measuredCoolRate(T), oven_cal::COOL_FAN_OFF.rate(T));
  }
}

// The feedforward holding duty holds the plant at the target it was computed for.
void test_cal_holding_duty_holds_the_plant(void) {
  for (float T : {80.0f, 150.0f, 220.0f}) {
    OvenPlant p;
    while (p.chamberTempC() < T) {
      p.step(1.0f, 1.0f, false, false, false);
    }
    const float d = steadyStateDuty(oven_cal::kDefaultModel, T, false);
    for (int i = 0; i < 1800; ++i) {
      p.step(1.0f, d, false, false, false);
    }
    TEST_ASSERT_FLOAT_WITHIN(6.0f, T, p.chamberTempC()); // settles at the target it holds for
  }
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_heating_reaches_a_loss_limited_ceiling);
  RUN_TEST(test_cooling_asymptotes_to_ambient);
  RUN_TEST(test_fan_on_ramps_faster);
  RUN_TEST(test_element_mass_causes_overshoot_after_cutoff);
  RUN_TEST(test_workpiece_lags_chamber);
  RUN_TEST(test_bay_warms_but_stays_below_chamber);
  RUN_TEST(test_bounded_under_adversarial_input);
  RUN_TEST(test_cal_heat_envelope_matches_plant);
  RUN_TEST(test_cal_cool_envelope_matches_plant);
  RUN_TEST(test_cal_holding_duty_holds_the_plant);
  return UNITY_END();
}
