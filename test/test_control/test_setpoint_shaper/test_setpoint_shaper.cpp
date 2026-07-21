// native_control suite — the PI loop's reference trajectory (design.md §5, the 2026-07-19 bench
// overshoot follow-up).
//
// Covers what SetpointShaper promises the run path: a RAMP_ASAP step becomes a paced climb that
// decelerates into its target (the soft landing that stops the element being overcharged), a timed
// ramp passes through untouched, the reference never leads the measurement without bound or exceeds
// the executor's setpoint, and every degenerate input (non-finite, dt <= 0, clock wrap, a garbage
// model) is total.
//
// The model here is a TOY with hand-picked constants, deliberately not oven_cal::kDefaultModel —
// the same posture test_heater_control takes with its ToyPlant. These assertions are about the
// shaper's arithmetic; keying them to the calibration file would make a future D6·tools
// regeneration silently rewrite what the test claims.
#include <unity.h>

#include <cmath>

#include "helpers/fake_clock.h"
#include "setpoint_shaper.h"
#include "thermal_math.h"

namespace {

constexpr uint32_t kDtMs = 500;
constexpr float kDtS = 0.5f;

// A flat 0.5 °C/s heat envelope and a flat 0.1 °C/s cool envelope (slope 0 → constant rate, the
// "idealized-linear" shape thermal_math documents), with the fan-on variant deliberately FASTER so
// a test can prove the FanPair is consulted. rateGain 0.5 °C/s per unit duty matches oven_cal's
// derivation (P/C_eff), so the feedforward numbers below are recognizable.
OvenModel toyModel() {
  OvenModel m{};
  m.heat.off = RateEnvelope{0.0f, 0.5f, 0.01f, 5.0f};
  m.heat.on = RateEnvelope{0.0f, 1.0f, 0.01f, 5.0f};
  m.cool.off = RateEnvelope{0.0f, 0.1f, 0.01f, 5.0f};
  m.cool.on = m.cool.off;
  const DutyModel d{0.004f, -0.1f, 0.5f};
  m.duty.off = d;
  m.duty.on = d;
  m.lag.off = LagParams{1.0f, 0.0f, 60.0f};
  m.lag.on = m.lag.off;
  return m;
}

// A config with the taper disabled, for the tests that are about pacing alone.
SetpointShaper::Config noTaper() {
  SetpointShaper::Config c{};
  c.approachTauS = 0.0f;
  return c;
}

// Drive `ticks` ticks holding the measurement pinned at `measured` (i.e. a plant that does not
// respond) or tracking the reference exactly, per `track`.
struct Driver {
  FakeClock clk;
  SetpointShaper shaper;
  OvenModel model = toyModel();
  float measured = 25.0f;

  explicit Driver(SetpointShaper::Config cfg = SetpointShaper::Config{})
      : clk(), shaper(clk, cfg) {}

  // One tick; `track` makes the (perfect) plant follow the reference exactly, which is the case
  // where the lead clamp must never bite.
  SetpointShaper::Shaped step(float spExec, bool convFan = false, bool track = true) {
    clk.advance(kDtMs);
    const SetpointShaper::Shaped s = shaper.update(spExec, measured, convFan, model);
    if (track) {
      measured = s.setpointC;
    }
    return s;
  }
};

// --- seeding ------------------------------------------------------------------------------------

// The reference starts at the MEASUREMENT, not at 0 and not at the target: a ramp is paced from
// where the oven actually is. The first tick establishes the dt baseline and paces nothing.
void test_seeds_at_the_measurement(void) {
  Driver d;
  d.measured = 31.0f;
  const SetpointShaper::Shaped s = d.step(90.0f);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 31.0f, s.setpointC);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, s.ratePerS);
}

// A chamber already hotter than the segment asks (the §15 cure resume, which is how the sibling
// TARGET_UNREACHABLE bug was found) must not be seeded a reference above the target — that would
// command heat into an oven that has already arrived.
void test_seed_never_exceeds_the_target(void) {
  Driver d;
  d.measured = 75.0f;
  const SetpointShaper::Shaped s = d.step(60.0f);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 60.0f, s.setpointC);
}

// reset() drops the reference: the next run re-seeds at its own starting temperature rather than
// inheriting the last one's.
void test_reset_reseeds(void) {
  Driver d;
  d.step(90.0f);
  for (int i = 0; i < 10; ++i) {
    d.step(90.0f);
  }
  TEST_ASSERT_TRUE(d.shaper.reference() > 27.0f); // 10 ticks × 0.5 °C/s × 0.5 s off the 25 °C seed

  d.shaper.reset();
  d.measured = 25.0f;
  const SetpointShaper::Shaped s = d.step(90.0f);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 25.0f, s.setpointC);
}

// --- pacing -------------------------------------------------------------------------------------

// The headline: an ASAP step is not handed to the PI as a step. Far from the target the reference
// advances at exactly the envelope rate × dt.
void test_asap_step_is_paced_at_the_envelope_rate(void) {
  Driver d(noTaper());
  d.step(200.0f); // seed at 25
  const SetpointShaper::Shaped s = d.step(200.0f);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 25.0f + 0.5f * kDtS, s.setpointC); // 0.5 °C/s × 0.5 s
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.5f, s.ratePerS);
}

// The pacing consults the fan-conditioned envelope (§6), not a single averaged plant.
void test_fan_on_paces_faster(void) {
  Driver off(noTaper());
  Driver on(noTaper());
  off.step(200.0f, false);
  on.step(200.0f, true);
  const float a = off.step(200.0f, false).setpointC;
  const float b = on.step(200.0f, true).setpointC;
  TEST_ASSERT_TRUE(b > a);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 25.0f + 1.0f * kDtS, b); // heat.on = 1.0 °C/s
}

// A FALLING setpoint is not shaped at all — it passes straight through. §5: "cool-down is passive
// (heater OFF + optional fan) → open-loop, not a PID-controlled descent". Pacing a descent would
// mean holding the element partly on to track it, which is a cooling controller this oven has no
// business having (and which would break the never-more-heat-than-today invariant).
void test_falling_setpoint_passes_through_unshaped(void) {
  Driver d(noTaper());
  d.measured = 100.0f;
  d.step(100.0f); // seed at 100
  const SetpointShaper::Shaped s = d.step(40.0f);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 40.0f, s.setpointC);
  TEST_ASSERT_TRUE(s.ratePerS < 0.0f); // reported as a step down, not paced
}

// TRANSPARENCY: a RAMP_OVER_TIME sweep slower than the envelope is returned unchanged, tick for
// tick. This is the claim that a timed ramp's behaviour does not change at all.
void test_timed_ramp_passes_through_unchanged(void) {
  Driver d;
  float sp = 25.0f;
  d.step(sp);
  for (int i = 0; i < 100; ++i) {
    sp += 0.05f * kDtS; // 0.05 °C/s — a tenth of the envelope
    const SetpointShaper::Shaped s = d.step(sp);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, sp, s.setpointC);
  }
}

// A paced ASAP ramp takes about as long as the CYD's own ETA integral says it will — the shaper and
// rampDurationSeconds() read the same envelope, so the controller tracks the trajectory the CYD
// projects. (Taper off: the soft landing deliberately adds time at the very end, checked below.)
void test_ramp_duration_matches_the_eta_integral(void) {
  Driver d(noTaper());
  const OvenModel m = toyModel();
  const float target = 100.0f;
  d.step(target);
  int ticks = 0;
  while (d.shaper.reference() < target - 0.01f && ticks < 100000) {
    d.step(target);
    ++ticks;
  }
  const float elapsed = static_cast<float>(ticks) * kDtS;
  const float eta = rampDurationSeconds(m.heat.off, 25.0f, target);
  TEST_ASSERT_FLOAT_WITHIN(2.0f * kDtS, eta, elapsed);
}

// --- the soft landing ---------------------------------------------------------------------------

// The taper is what actually fixes the overshoot: near the target the reference must close SLOWER
// than the envelope, so the commanded rate — and with it the feedforward duty — is already falling
// when the chamber arrives, instead of the element being driven flat-out until the error hits zero.
void test_approach_tapers_near_the_target(void) {
  SetpointShaper::Config cfg{};
  cfg.approachTauS = 50.0f;
  Driver d(cfg);
  const float target = 100.0f;
  d.step(target);

  float farRate = 0.0f;
  float nearRate = 0.0f;
  for (int i = 0; i < 100000; ++i) {
    const SetpointShaper::Shaped s = d.step(target);
    const float remaining = target - s.setpointC;
    if (remaining > 60.0f) {
      farRate = s.ratePerS; // still envelope-limited out here
    }
    if (remaining < 5.0f && remaining > 1.0f) {
      nearRate = s.ratePerS; // taper-limited on the approach
    }
    if (remaining <= 0.6f) {
      break;
    }
  }
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.5f, farRate); // envelope rate far out
  TEST_ASSERT_TRUE(nearRate > 0.0f);              // still closing…
  TEST_ASSERT_TRUE(nearRate < 0.2f);              // …but well under the envelope: duty easing off
}

// Arrival is exact and sticky — the taper alone would approach asymptotically forever, so the
// arrive band has to land it. Checked at a coarse and a fine tick, since the snap interacts with
// dt.
void test_arrival_is_exact_and_never_overshoots(void) {
  const uint32_t dts[] = {50U, 1000U};
  for (uint32_t dt : dts) {
    FakeClock clk;
    SetpointShaper sh(clk);
    const OvenModel m = toyModel();
    const float target = 80.0f;
    float measured = 25.0f;
    float maxRef = 0.0f;
    for (int i = 0; i < 20000; ++i) {
      clk.advance(dt);
      const SetpointShaper::Shaped s = sh.update(target, measured, false, m);
      measured = s.setpointC; // perfect tracking
      if (s.setpointC > maxRef) {
        maxRef = s.setpointC;
      }
      TEST_ASSERT_TRUE(s.setpointC <= target + 0.0001f); // never past the executor's setpoint
    }
    TEST_ASSERT_FLOAT_WITHIN(0.001f, target, maxRef); // and it does get there
    TEST_ASSERT_FLOAT_WITHIN(0.001f, target, sh.reference());
  }
}

// --- the lead clamp -----------------------------------------------------------------------------

// A plant that cannot keep up must not let the reference run away — that would restore the
// saturated step this class exists to remove, and wind the integrator up behind it.
void test_lead_clamp_bounds_a_stalled_plant(void) {
  SetpointShaper::Config cfg{};
  cfg.maxLeadC = 10.0f;
  cfg.approachTauS = 0.0f;
  Driver d(cfg);
  d.measured = 25.0f;
  d.step(200.0f, false, false); // seed; measurement pinned from here on
  for (int i = 0; i < 500; ++i) {
    d.step(200.0f, false, false);
  }
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 35.0f, d.shaper.reference()); // 25 + maxLeadC, and no further
}

// …and it releases as soon as the plant moves: the clamp bounds the error, it does not cap the run.
void test_lead_clamp_releases_when_the_plant_moves(void) {
  SetpointShaper::Config cfg{};
  cfg.maxLeadC = 10.0f;
  cfg.approachTauS = 0.0f;
  Driver d(cfg);
  d.measured = 25.0f;
  for (int i = 0; i < 500; ++i) {
    d.step(200.0f, false, false);
  }
  d.measured = 60.0f;
  const SetpointShaper::Shaped s = d.step(200.0f, false, false);
  TEST_ASSERT_TRUE(s.setpointC > 35.0f);
  TEST_ASSERT_TRUE(s.setpointC <= 70.0f + 0.001f);
}

// A perfectly tracking plant never trips the clamp — a legitimately fast ramp is not throttled.
void test_lead_clamp_is_inert_when_the_plant_keeps_up(void) {
  SetpointShaper::Config cfg{};
  cfg.maxLeadC = 10.0f;
  Driver d(cfg);
  // Long enough to cover the taper's exponential tail (τ = 50 s from 65 °C out to the arrive band).
  for (int i = 0; i < 1200; ++i) {
    const SetpointShaper::Shaped s = d.step(90.0f, false, true);
    TEST_ASSERT_TRUE(s.setpointC <= d.measured + 10.0f + 0.001f);
  }
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 90.0f, d.shaper.reference()); // it still arrives
}

// --- totality -----------------------------------------------------------------------------------

// Blind control passes THROUGH: HeaterControl treats a non-finite setpoint/measurement as a stop
// condition, and sanitizing it here into a finite reference would defeat that fail-safe (§4).
void test_non_finite_inputs_pass_through(void) {
  Driver d;
  d.step(90.0f);
  const OvenModel m = toyModel();

  d.clk.advance(kDtMs);
  SetpointShaper::Shaped s = d.shaper.update(NAN, 40.0f, false, m);
  TEST_ASSERT_TRUE(std::isnan(s.setpointC));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, s.ratePerS);

  d.clk.advance(kDtMs);
  s = d.shaper.update(90.0f, NAN, false, m);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 90.0f, s.setpointC); // the setpoint, unshaped
  TEST_ASSERT_EQUAL_FLOAT(0.0f, s.ratePerS);

  d.clk.advance(kDtMs);
  s = d.shaper.update(INFINITY, 40.0f, false, m);
  TEST_ASSERT_FALSE(std::isfinite(s.setpointC));

  // …and it recovers, re-seeding at the measurement.
  d.clk.advance(kDtMs);
  s = d.shaper.update(90.0f, 41.0f, false, m);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 41.0f, s.setpointC);
}

// A tick with no time elapsed holds the reference still and reports no rate (no divide by ~0).
void test_zero_dt_holds_the_reference(void) {
  Driver d;
  d.step(200.0f);
  const float a = d.step(200.0f).setpointC;
  const OvenModel m = toyModel();
  const SetpointShaper::Shaped s =
      d.shaper.update(200.0f, d.measured, false, m); // no clock advance
  TEST_ASSERT_FLOAT_WITHIN(0.001f, a, s.setpointC);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, s.ratePerS);
}

// The millis() wrap must be a normal tick, not a 49-day dt (wrap-safe uint32 subtraction, the same
// property HeaterControl relies on).
void test_clock_wrap_is_an_ordinary_tick(void) {
  FakeClock clk;
  clk.now = 0xFFFFFF00U;
  SetpointShaper sh(clk);
  const OvenModel m = toyModel();
  sh.update(200.0f, 25.0f, false, m);
  clk.advance(0x200U); // wraps past 2^32
  const SetpointShaper::Shaped s = sh.update(200.0f, 25.0f, false, m);
  TEST_ASSERT_TRUE(std::isfinite(s.setpointC));
  TEST_ASSERT_TRUE(s.setpointC < 26.0f); // 0.5 s of pacing, not 49 days of it
}

// An unusable model (the envelope cannot say how fast this plant moves) must not stall the oven:
// it falls back to the unshaped setpoint — exactly the behaviour that shipped before this class.
void test_degenerate_model_falls_back_to_the_raw_setpoint(void) {
  SetpointShaper::Config cfg{};
  cfg.maxLeadC = 40.0f;
  FakeClock clk;
  SetpointShaper sh(clk, cfg);
  OvenModel m = toyModel();
  m.heat.off = RateEnvelope{0.0f, 0.0f, 0.0f, 0.0f}; // no rate at all
  sh.update(150.0f, 25.0f, false, m);
  clk.advance(kDtMs);
  const SetpointShaper::Shaped s = sh.update(150.0f, 25.0f, false, m);
  // The fallback hands the raw setpoint through; the lead clamp — the one bound that does NOT
  // depend on the model — still applies, so what the PI sees is measured + maxLeadC. Both layers
  // doing their job: an unusable model cannot stall the oven, and cannot uncork it either.
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 65.0f, s.setpointC);
}

// --- the feedforward primitive this feeds (thermal_math.h) --------------------------------------

// rampFeedforwardDuty: holding duty at rate 0, plus rate/rateGain while climbing, clamped 0..1, and
// total against a degenerate gain.
void test_ramp_feedforward_duty(void) {
  const OvenModel m = toyModel();
  const float hold = steadyStateDuty(m, 100.0f, false); // 0.004·100 − 0.1 = 0.3
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.3f, hold);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, hold, rampFeedforwardDuty(m, 100.0f, 0.0f, false));
  // climbing at 0.25 °C/s with rateGain 0.5 → +0.5 duty
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.8f, rampFeedforwardDuty(m, 100.0f, 0.25f, false));
  // a falling reference asks for less than the holding duty
  TEST_ASSERT_TRUE(rampFeedforwardDuty(m, 100.0f, -0.1f, false) < hold);
  // clamped to the actuator's range at both ends
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, rampFeedforwardDuty(m, 100.0f, 10.0f, false));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, rampFeedforwardDuty(m, 100.0f, -10.0f, false));

  OvenModel bad = m;
  bad.duty.off.rateGain = 0.0f; // cannot convert a rate into duty → holding term only
  TEST_ASSERT_FLOAT_WITHIN(0.001f, hold, rampFeedforwardDuty(bad, 100.0f, 0.25f, false));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, hold, rampFeedforwardDuty(m, 100.0f, NAN, false));
}

} // namespace

void setUp(void) {}
void tearDown(void) {}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_seeds_at_the_measurement);
  RUN_TEST(test_seed_never_exceeds_the_target);
  RUN_TEST(test_reset_reseeds);
  RUN_TEST(test_asap_step_is_paced_at_the_envelope_rate);
  RUN_TEST(test_fan_on_paces_faster);
  RUN_TEST(test_falling_setpoint_passes_through_unshaped);
  RUN_TEST(test_timed_ramp_passes_through_unchanged);
  RUN_TEST(test_ramp_duration_matches_the_eta_integral);
  RUN_TEST(test_approach_tapers_near_the_target);
  RUN_TEST(test_arrival_is_exact_and_never_overshoots);
  RUN_TEST(test_lead_clamp_bounds_a_stalled_plant);
  RUN_TEST(test_lead_clamp_releases_when_the_plant_moves);
  RUN_TEST(test_lead_clamp_is_inert_when_the_plant_keeps_up);
  RUN_TEST(test_non_finite_inputs_pass_through);
  RUN_TEST(test_zero_dt_holds_the_reference);
  RUN_TEST(test_clock_wrap_is_an_ordinary_tick);
  RUN_TEST(test_degenerate_model_falls_back_to_the_raw_setpoint);
  RUN_TEST(test_ramp_feedforward_duty);
  return UNITY_END();
}
