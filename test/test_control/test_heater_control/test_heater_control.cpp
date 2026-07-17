// native_control suite — the PI heater control loop (design.md §5, backlog A5).
//
// Drives a real HeaterControl over a FakeClock against a toy first-order thermal plant
// (dT/dt = rateGain·duty − loss·(T − ambient)), the same shape design.md §5/§6 models the
// oven as. Covers: zero steady-state error, anti-windup under saturation, the caller-supplied
// feedforward hook (fed the real steadyStateDuty() inverse-plant term), the one-sided-heater
// clamp, fail-safe on non-finite input + reset(), the dt baseline on the first tick, and the
// inert Kd seam.
#include <unity.h>

#include <cmath>

#include "heater_control.h"
#include "helpers/fake_clock.h"
#include "thermal_math.h" // steadyStateDuty / DutyModel / OvenModel — the feedforward model (§6)

namespace {

// Toy first-order plant: a lumped body heated by duty and leaking toward ambient. Steady state
// holds at duty_ss(T) = loss·(T − ambient) / rateGain, which the DutyModel below inverts so the
// feedforward term alone nearly holds temperature and feedback only trims the residual.
struct ToyPlant {
  float tempC = 25.0f;
  float ambientC = 25.0f;
  float rateGain = 1.0f; // °C/s per unit duty
  float loss = 0.01f;    // 1/s toward ambient (τ ≈ 100 s); max hold ≈ ambient + rateGain/loss

  void step(float duty, float dtS) { tempC += (rateGain * duty - loss * (tempC - ambientC)) * dtS; }
};

// The inverse-plant feedforward model matching a ToyPlant, wrapped in an OvenModel so the test
// exercises the real steadyStateDuty() path (both fan variants identical — fan state is moot here).
OvenModel ffModelFor(const ToyPlant &p) {
  const DutyModel dm{p.loss / p.rateGain, -p.loss * p.ambientC / p.rateGain, p.rateGain};
  OvenModel m{};
  m.duty.off = dm;
  m.duty.on = dm;
  return m;
}

constexpr uint32_t kDtMs = 1000;
constexpr float kDtS = 1.0f;

// Close the loop for `ticks` steps at kDtMs. `ff` selects whether the feedforward hook is fed the
// real steadyStateDuty() term or 0. Records the peak temperature reached (for overshoot checks).
float runLoop(HeaterControl &pi, FakeClock &clk, ToyPlant &plant, float setpoint, int ticks,
              const OvenModel *ff, float *peakOut = nullptr) {
  float peak = plant.tempC;
  for (int i = 0; i < ticks; ++i) {
    const float ffDuty = ff ? steadyStateDuty(*ff, setpoint, false) : 0.0f;
    const float duty = pi.update(setpoint, plant.tempC, ffDuty);
    plant.step(duty, kDtS);
    clk.advance(kDtMs);
    if (plant.tempC > peak) {
      peak = plant.tempC;
    }
  }
  if (peakOut != nullptr) {
    *peakOut = peak;
  }
  return plant.tempC;
}

} // namespace

void setUp(void) {}
void tearDown(void) {}

// PI drives the plant to setpoint with no steady-state offset (the integrator kills the residual
// a pure-P loop would leave).
void test_converges_zero_steady_state_error(void) {
  FakeClock clk;
  HeaterControl pi(clk);
  ToyPlant plant; // starts at 25 °C
  const float finalT = runLoop(pi, clk, plant, 100.0f, 4000, nullptr);
  TEST_ASSERT_FLOAT_WITHIN(1.0f, 100.0f, finalT);
  TEST_ASSERT_TRUE(std::isfinite(pi.integrator()));
}

// Ramping from cold to a reachable setpoint pins the duty at 1.0 for the whole lower stretch
// (large positive error). Conditional integration freezes the integrator through that
// saturation instead of accumulating it, so the approach settles with only a small overshoot —
// the observable symptom a wound-up integrator would blow out into a large lurch past target.
void test_anti_windup_small_overshoot_on_saturated_ramp(void) {
  FakeClock clk;
  HeaterControl pi(clk);
  ToyPlant plant; // 25 °C — the ramp to 100 saturates duty while temp < ~50 °C

  float peak = 0.0f;
  const float finalT = runLoop(pi, clk, plant, 100.0f, 3000, nullptr, &peak);
  TEST_ASSERT_TRUE(std::isfinite(pi.integrator()));
  TEST_ASSERT_FLOAT_WITHIN(1.5f, 100.0f, finalT);
  TEST_ASSERT_TRUE(peak < 106.0f); // small overshoot, not a wound-up lurch past target
}

// The feedforward hook carries the holding duty, so feedback barely has to integrate: holding at
// setpoint, the FF loop keeps the integrator near zero while a pure-feedback loop must build a
// large integrator to supply the same duty.
void test_feedforward_carries_holding_duty(void) {
  FakeClock clk;
  ToyPlant plant;
  plant.tempC = 100.0f; // start already at setpoint
  const OvenModel model = ffModelFor(plant);

  HeaterControl piFf(clk);
  const float finalFf = runLoop(piFf, clk, plant, 100.0f, 300, &model);
  TEST_ASSERT_FLOAT_WITHIN(1.0f, 100.0f, finalFf);       // FF holds temperature
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.75f, piFf.duty());   // duty ≈ steadyStateDuty(100) = 0.75
  TEST_ASSERT_TRUE(std::fabs(piFf.integrator()) < 5.0f); // feedback barely integrates

  FakeClock clk2;
  ToyPlant plant2;
  plant2.tempC = 100.0f;
  HeaterControl piNoFf(clk2);
  runLoop(piNoFf, clk2, plant2, 100.0f, 2000, nullptr);
  // Without FF the integrator alone must supply the whole ~0.75 holding duty (ki·I ≈ 0.75 => I ≈
  // 375).
  TEST_ASSERT_TRUE(std::fabs(piNoFf.integrator()) > 30.0f);
}

// One-sided heater: a measurement above setpoint commands 0 (never "cooling"); a measurement far
// below saturates at 1.0. Duty never escapes [0, 1].
void test_one_sided_clamp(void) {
  FakeClock clk;
  HeaterControl pi(clk);
  pi.update(100.0f, 100.0f, 0.0f); // baseline tick
  for (int i = 0; i < 50; ++i) {
    clk.advance(kDtMs);
    const float duty = pi.update(100.0f, 150.0f, 0.0f); // measured well above setpoint
    TEST_ASSERT_FLOAT_WITHIN(1.0e-6f, 0.0f, duty);
  }
  clk.advance(kDtMs);
  const float hot = pi.update(100.0f, 0.0f, 0.0f); // measured far below → saturate high
  TEST_ASSERT_TRUE(hot >= 0.0f && hot <= 1.0f);
  TEST_ASSERT_FLOAT_WITHIN(1.0e-3f, 1.0f, hot);
}

// Blind control fails safe: a non-finite setpoint or measurement commands OFF and freezes the
// integrator; reset() clears everything.
void test_fail_safe_nonfinite_and_reset(void) {
  FakeClock clk;
  HeaterControl pi(clk);
  ToyPlant plant;
  runLoop(pi, clk, plant, 100.0f, 200, nullptr); // build some integrator
  const float saved = pi.integrator();
  TEST_ASSERT_TRUE(std::fabs(saved) > 0.0f);

  clk.advance(kDtMs);
  TEST_ASSERT_FLOAT_WITHIN(1.0e-6f, 0.0f, pi.update(100.0f, NAN, 0.0f)); // faulted TC → OFF
  TEST_ASSERT_EQUAL_FLOAT(saved, pi.integrator());                       // frozen, not corrupted
  clk.advance(kDtMs);
  TEST_ASSERT_FLOAT_WITHIN(1.0e-6f, 0.0f, pi.update(INFINITY, 100.0f, 0.0f)); // bad setpoint → OFF

  pi.reset();
  TEST_ASSERT_EQUAL_FLOAT(0.0f, pi.integrator());
  TEST_ASSERT_EQUAL_FLOAT(0.0f, pi.duty());
}

// The first tick after construction/reset establishes the dt baseline (dt ≈ 0), so it does no
// integration — the output is pure proportional (+ FF) that tick.
void test_dt_baseline_first_tick_no_integration(void) {
  FakeClock clk;
  HeaterControl pi(clk);
  const float duty = pi.update(100.0f, 90.0f, 0.0f); // error +10, kp=0.02 → 0.2
  TEST_ASSERT_EQUAL_FLOAT(0.0f, pi.integrator());    // no integration on the baseline tick
  TEST_ASSERT_FLOAT_WITHIN(1.0e-4f, 0.2f, duty);

  clk.advance(kDtMs);
  pi.update(100.0f, 90.0f, 0.0f); // now dt > 0 → integrator starts moving
  TEST_ASSERT_TRUE(pi.integrator() > 0.0f);
}

// The Kd seam is derivative-on-measurement and off by default: kd=0 leaves the output as pure PI,
// while a positive kd subtracts a term proportional to the measurement's rate of rise (opposing a
// climbing temperature). Same hand-driven trajectory through both proves the seam is wired.
void test_kd_seam_inert_by_default_active_when_set(void) {
  HeaterControl::Config cfgD;
  cfgD.gains.kd = 0.1f;

  FakeClock clkPi, clkPid;
  HeaterControl pi(clkPi);         // kd = 0 (default)
  HeaterControl pid(clkPid, cfgD); // kd = 0.1

  // Setpoint 115 keeps both duties in the open (0, 1) interval so the clamp can't mask the D
  // term. Baseline tick at a steady measurement (no derivative yet) → identical output.
  pi.update(115.0f, 80.0f, 0.0f);
  pid.update(115.0f, 80.0f, 0.0f);
  TEST_ASSERT_EQUAL_FLOAT(pi.duty(), pid.duty()); // identical until the measurement moves

  // Next tick: measurement has risen 5 °C over 1 s → the D term pulls the PID duty below the PI
  // duty.
  clkPi.advance(kDtMs);
  clkPid.advance(kDtMs);
  const float dutyPi = pi.update(115.0f, 85.0f, 0.0f);
  const float dutyPid = pid.update(115.0f, 85.0f, 0.0f);
  TEST_ASSERT_TRUE(dutyPid < dutyPi);
  // D contribution ≈ kd · dMeas/dt = 0.1 · 5 = 0.5 below the PI duty.
  TEST_ASSERT_FLOAT_WITHIN(1.0e-3f, 0.5f, dutyPi - dutyPid);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_converges_zero_steady_state_error);
  RUN_TEST(test_anti_windup_small_overshoot_on_saturated_ramp);
  RUN_TEST(test_feedforward_carries_holding_duty);
  RUN_TEST(test_one_sided_clamp);
  RUN_TEST(test_fail_safe_nonfinite_and_reset);
  RUN_TEST(test_dt_baseline_first_tick_no_integration);
  RUN_TEST(test_kd_seam_inert_by_default_active_when_set);
  return UNITY_END();
}
