// native_control suite — ControllerRunPath, the controller's per-loop control composition
// (design.md §5/§11; backlog A10, extended by the §5 overshoot work).
//
// Why this suite exists: until now the run path was only ever exercised THROUGH test_sim_run's
// closed loop. That is the right test for "does the oven end up in the right place", and the wrong
// one for "is each term correct" — a closed loop hides a wrong feedforward behind the integrator
// quietly compensating for it, and reports success either way. This drives the same object graph
// with the plant replaced by a hand-driven FakeThermocouples, so each collaborator's contribution
// is asserted directly:
//   - the PI tracks the SHAPED reference, not the executor's raw setpoint;
//   - the feedforward is rampFeedforwardDuty() at the shaped setpoint, to the number;
//   - integration is gated to holds (§5: feedforward owns ramps, the integrator owns holds);
//   - a stop resets BOTH the PID and the shaper;
//   - the shaped setpoint never exceeds the executor's — the "can only reduce commanded heat"
//   claim.
// It also pins the behaviours this class already owned that nothing directly tested: the §15 door
// abort, arm/disarm around RUNNING, and executor-fault routing into SafetySupervisor::trip().
#include <unity.h>

#include <cmath>

#include "controller_link.h"
#include "cyd_link.h"
#include "frame_link.h"
#include "heater_actuator.h"
#include "heater_control.h"
#include "helpers/fake_clock.h"
#include "helpers/fake_contactor.h"
#include "helpers/fake_door_sensor.h"
#include "helpers/fake_heater_switch.h"
#include "helpers/fake_thermocouples.h"
#include "helpers/pipe_transport.h"
#include "link_params.h"
#include "message_router.h"
#include "messages.h"
#include "oven.pb.h"
#include "oven_cal.h"
#include "oven_safety.h"
#include "profile_executor.h"
#include "run_path.h"
#include "safety_supervisor.h"
#include "setpoint_shaper.h"
#include "thermal_math.h"

namespace {

constexpr uint32_t kCydNonce = 0xC1D0B100;
constexpr uint32_t kCtrlNonce = 0xC710B100;
constexpr uint32_t kSession = 0xBE0CB100;
constexpr uint32_t kStepMs = 500;

// test_sim_run's rig with the plant taken out: the temperatures are whatever the test says they
// are. Everything else — both link facades over one pipe, the real executor/PID/shaper/supervisor —
// is the production object graph.
struct Rig {
  LoopbackPipe pipe;
  FakeClock clk;
  protocol::MessageRouter cyd_router;
  protocol::FrameLink cyd_link;
  protocol::CydLink cyd;
  protocol::MessageRouter ctrl_router;
  protocol::FrameLink ctrl_link;
  ControllerLink ctrl;
  FakeHeaterSwitch heater_sw;
  FakeContactor contactor;
  HeaterActuator heater;
  ProfileExecutor exec;
  HeaterControl pid;
  SetpointShaper shaper;
  FakeThermocouples tc;
  FakeDoorSensor door;
  SafetySupervisor safety;
  ControllerRunPath runpath;

  Rig()
      : cyd_router(), cyd_link(pipe.a(), TF_MASTER, cyd_router), cyd(cyd_link, clk), ctrl_router(),
        ctrl_link(pipe.b(), TF_SLAVE, ctrl_router), ctrl(ctrl_link, clk), heater(heater_sw, clk),
        exec(clk), pid(clk), shaper(clk), safety(ctrl, heater, contactor, tc, clk),
        runpath(exec, pid, shaper, safety, heater, ctrl, tc, door, oven_cal::kDefaultModel) {
    cyd_router.setObserver(cyd);
    ctrl_router.setObserver(ctrl);
    ctrl.setExecutor(exec);
  }

  void controllerLoop() {
    ctrl_link.poll();
    ctrl.service();
    runpath.tick();
    heater.tick();
    safety.tick();
  }

  void exchange() {
    controllerLoop();
    cyd_link.poll();
    cyd.service();
  }

  void tick() {
    clk.advance(kStepMs);
    exchange();
  }

  void run(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += kStepMs) {
      tick();
    }
  }

  void startRun(const oven_Recipe &rec) {
    cyd.begin(kCydNonce);
    ctrl.begin(kCtrlNonce);
    exchange();
    cyd.sender().sendRecipe(rec);
    exchange();
    oven_Start st = oven_Start_init_default;
    st.session = kSession;
    st.recipe_id = rec.id;
    cyd.sender().sendStart(st);
    exchange();
    cyd.heartbeat().setSession(kSession);
    cyd.heartbeat().setEnable(true);
  }

  // Hold every channel at one temperature (the cure control sensor is wall(0), reflow's is the
  // workpiece — setAll covers both without the test caring which mode it is in).
  void setTemp(float c) { tc.setAll(c); }

  // A stand-in for a plant that follows its reference: the measurement trails the shaped setpoint
  // by a fixed lag. Enough for the executor to make progress (a frozen measurement would trip the
  // per-segment rate-floor watchdog, which is a different test's subject) while still leaving the
  // positive error a plain PI would wind up on.
  float trackedTemp() const { return runpath.shapedSetpointC() - kTrackLagC; }
  static constexpr float kTrackLagC = 1.0f;
};

oven_Segment seg(oven_Interp interp, float heatC, uint32_t durMs, bool fan, bool uv, bool motor) {
  oven_Segment s = oven_Segment_init_default;
  s.interp = interp;
  s.heat_c = heatC;
  s.dur_ms = durMs;
  s.conv_fan = fan;
  s.uv = uv;
  s.motor = motor;
  return s;
}

// An ASAP cure ramp to 60 °C then a hold: the shape that produced the bench overshoot, and the one
// where the shaper is not transparent.
oven_Recipe asapCure() {
  oven_Recipe r = oven_Recipe_init_default;
  r.id = 55;
  r.mode = oven_Mode_MODE_CURE;
  r.segments_count = 2;
  r.segments[0] = seg(oven_Interp_INTERP_RAMP_ASAP, 60.0f, 200000, true, true, true);
  r.segments[1] = seg(oven_Interp_INTERP_HOLD, 60.0f, 120000, true, true, true);
  return r;
}

// The same profile with a hold long enough to observe steady-state behaviour in.
oven_Recipe longHoldCure() {
  oven_Recipe r = asapCure();
  r.id = 56;
  r.segments[1] = seg(oven_Interp_INTERP_HOLD, 60.0f, 1800000, true, true, true);
  return r;
}

} // namespace

void setUp(void) {}
void tearDown(void) {}

// The composition, to the number: the duty commanded onto the actuator is exactly what the PI
// returns for the SHAPED setpoint fed the trajectory feedforward — recomputed here against a
// reference HeaterControl driven in lockstep. If the run path ever hands the PI the executor's raw
// setpoint, or evaluates the feedforward at the measurement instead of the shaped setpoint, the
// duties diverge on the first tick of the ramp.
void test_duty_is_the_pi_output_for_the_shaped_setpoint(void) {
  Rig r;
  r.setTemp(25.0f);
  r.startRun(asapCure());

  const HeaterControl::Gains g{}; // the defaults the run path's PID is built with
  for (int i = 0; i < 60; ++i) {
    r.tick();
    const ProfileExecutor::Output &o = r.runpath.output();
    // Skip ticks the supervisor overrode: it runs LAST and has the final word over duty (§4), so
    // an unauthorized tick reads 0 no matter what the loop asked for. That is its own test's
    // subject (test_safety_supervisor); this one is about what the loop REQUESTS.
    if (o.safe || !r.ctrl.authorized()) {
      continue;
    }
    // Recomputed from the rig's OWN post-tick state, so no lockstep mirror can drift out of phase
    // with it: duty = clamp(ff(shaped, rate) + kp·(shaped − measured) + ki·I).
    const float measured = r.tc.wallC[0];
    const float shaped = r.runpath.shapedSetpointC();
    const float ff =
        rampFeedforwardDuty(oven_cal::kDefaultModel, shaped, r.shaper.ratePerS(), o.convFan);
    float expected = ff + g.kp * (shaped - measured) + g.ki * r.pid.integrator();
    expected = expected < 0.0f ? 0.0f : (expected > 1.0f ? 1.0f : expected);
    TEST_ASSERT_FLOAT_WITHIN(1.0e-5f, expected, r.heater.duty());

    // Evaluated at the SHAPED setpoint, not the measurement — the distinction that makes this
    // feedforward a trajectory term rather than the holding term A5 shipped with.
    TEST_ASSERT_TRUE(shaped > measured);
    TEST_ASSERT_TRUE(ff > steadyStateDuty(oven_cal::kDefaultModel, measured, o.convFan));

    r.setTemp(r.trackedTemp()); // a plant that follows, so the executor is not stalled
  }
}

// The reference is genuinely SHAPED, not passed through: on an ASAP ramp it sits between the
// measurement and the executor's target, and climbs. (The invariant the run path advertises —
// "never above the executor's setpoint, so this can only ever reduce commanded heat" — is asserted
// on every tick.)
void test_shaped_setpoint_is_between_the_measurement_and_the_target(void) {
  Rig r;
  r.setTemp(25.0f);
  r.startRun(asapCure());

  float first = 0.0f;
  float last = 0.0f;
  for (int i = 0; i < 40; ++i) {
    r.tick();
    const ProfileExecutor::Output &o = r.runpath.output();
    if (o.safe) {
      continue;
    }
    last = r.runpath.shapedSetpointC();
    if (first == 0.0f) {
      first = last;
    }
    TEST_ASSERT_TRUE(last <= r.safety.clampSetpoint(o.setpointC) + 1.0e-4f);
  }
  TEST_ASSERT_TRUE(first < 60.0f); // it did NOT hand the PI the 60 °C step
  TEST_ASSERT_TRUE(last > first);  // …and it is climbing toward it
}

// Feedforward owns ramps, the integrator owns holds (§5). While the reference is moving the
// integrator must not accumulate — that accumulation is what kept the element charged past arrival.
// Once the reference arrives (rate exactly 0), integration resumes.
void test_integration_is_gated_to_holds(void) {
  Rig r;
  r.setTemp(25.0f);
  r.startRun(longHoldCure());

  // Ramping: the reference is moving every tick, so accumulation stays off.
  for (int i = 0; i < 60; ++i) {
    r.tick();
    r.setTemp(r.trackedTemp());
    if (!r.runpath.output().safe) {
      TEST_ASSERT_FALSE(r.pid.integrating());
    }
  }
  TEST_ASSERT_EQUAL_FLOAT(0.0f, r.pid.integrator());

  // Let it arrive and settle into the hold: the reference stops moving, so a standing error is now
  // worth integrating — and the loop must be able to build one (a hold is where accuracy matters).
  for (int i = 0; i < 2000 && r.shaper.ratePerS() != 0.0f; ++i) {
    r.tick();
    r.setTemp(r.trackedTemp());
  }
  TEST_ASSERT_FALSE(r.safety.faulted()); // a slow-but-arriving approach is not a stall
  TEST_ASSERT_TRUE(r.pid.integrating());
  r.tc.setAll(59.0f); // a standing 1 °C shortfall during the hold
  r.run(30000);
  TEST_ASSERT_TRUE(r.pid.integrator() > 0.0f);
}

// A stop resets both pieces of loop state. Without the shaper reset, the next run would start from
// the last run's reference instead of from the oven's actual temperature.
void test_safe_output_resets_pid_and_shaper(void) {
  Rig r;
  r.setTemp(25.0f);
  r.startRun(asapCure());
  for (int i = 0; i < 60; ++i) {
    r.tick();
    r.setTemp(r.trackedTemp());
  }
  TEST_ASSERT_TRUE(r.runpath.shapedSetpointC() > 25.0f);

  // Abort → the executor goes safe → the run path commands OFF and drops both.
  r.cyd.sendAbort();
  r.exchange();
  r.tick();
  TEST_ASSERT_TRUE(r.runpath.output().safe);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, r.heater.duty());
  TEST_ASSERT_EQUAL_FLOAT(0.0f, r.pid.integrator());

  // A fresh run re-seeds the reference at the CURRENT temperature, not the old one.
  r.setTemp(48.0f);
  oven_Recipe rec = asapCure();
  rec.id = 66;
  r.cyd.sender().sendRecipe(rec);
  r.exchange();
  oven_Start st = oven_Start_init_default;
  st.session = kSession;
  st.recipe_id = rec.id;
  r.cyd.sender().sendStart(st);
  r.exchange();
  r.tick();
  TEST_ASSERT_FLOAT_WITHIN(1.5f, 48.0f, r.runpath.shapedSetpointC());
}

// A timed ramp is transparent — the executor's setpoint reaches the PI unchanged, tick for tick.
// This is the "nothing about RAMP_OVER_TIME behaviour changes" claim, asserted end to end rather
// than only at the shaper's own seam.
void test_timed_ramp_reaches_the_pi_unshaped(void) {
  Rig r;
  r.setTemp(25.0f);
  oven_Recipe rec = oven_Recipe_init_default;
  rec.id = 77;
  rec.mode = oven_Mode_MODE_CURE;
  rec.segments_count = 2;
  // 55 °C over 600 s ≈ 0.09 °C/s — far under the plant's envelope, so pacing never binds.
  rec.segments[0] = seg(oven_Interp_INTERP_RAMP_OVER_TIME, 80.0f, 600000, true, true, true);
  rec.segments[1] = seg(oven_Interp_INTERP_HOLD, 80.0f, 60000, true, true, true);
  r.startRun(rec);

  for (int i = 0; i < 60; ++i) {
    r.tick();
    const ProfileExecutor::Output &o = r.runpath.output();
    if (o.safe) {
      continue;
    }
    r.setTemp(r.runpath.shapedSetpointC()); // a plant that tracks perfectly
    TEST_ASSERT_FLOAT_WITHIN(0.01f, r.safety.clampSetpoint(o.setpointC),
                             r.runpath.shapedSetpointC());
  }
}

// --- behaviours this class already owned, never directly tested ---------------------------------

// §15: a door-open during a run safes and ends it to IDLE, without faulting (§22 excludes it), and
// the edge is latched for the UI.
void test_door_open_ends_the_run_without_faulting(void) {
  Rig r;
  r.setTemp(25.0f);
  r.startRun(asapCure());
  r.run(10000);
  TEST_ASSERT_EQUAL(oven_RunState_RUN_STATE_RUNNING, r.exec.state());

  r.door.open = true;
  r.tick();
  TEST_ASSERT_NOT_EQUAL(oven_RunState_RUN_STATE_RUNNING, r.exec.state());
  TEST_ASSERT_EQUAL_FLOAT(0.0f, r.heater.duty()); // OFF on the same tick
  TEST_ASSERT_FALSE(r.safety.faulted());          // expected event, not an alarm
  TEST_ASSERT_TRUE(r.runpath.doorOpen());
  TEST_ASSERT_TRUE(r.runpath.doorAborted());
  r.runpath.clearDoorAbort();
  TEST_ASSERT_FALSE(r.runpath.doorAborted());
}

// The supervisor's L3 checks are armed for the RUNNING lifetime and disarmed when it ends.
void test_arms_and_disarms_the_supervisor_around_the_run(void) {
  Rig r;
  r.setTemp(25.0f);
  TEST_ASSERT_FALSE(r.runpath.armed());
  r.startRun(asapCure());
  r.tick();
  TEST_ASSERT_TRUE(r.runpath.armed());

  r.cyd.sendAbort();
  r.exchange();
  r.tick();
  TEST_ASSERT_FALSE(r.runpath.armed());
}

// An executor fault routes into the supervisor's latch — without this the run would stop but the
// contactor would stay closed, since the supervisor only self-trips on its own L3 checks.
void test_executor_fault_routes_into_the_supervisor(void) {
  Rig r;
  r.setTemp(25.0f);
  r.startRun(asapCure());
  r.run(5000);
  TEST_ASSERT_FALSE(r.safety.faulted());

  // Lose the control channel: the executor refuses to run blind (SENSOR_FAULT, §4).
  r.tc.workpieceFault = true;
  for (int i = 0; i < FakeThermocouples::kWalls; ++i) {
    r.tc.wallFault[i] = true;
  }
  r.run(5000);
  TEST_ASSERT_TRUE(r.safety.faulted());
  TEST_ASSERT_EQUAL(oven_FaultCode_FAULT_SENSOR_FAULT, r.safety.faultCode());
  TEST_ASSERT_EQUAL_FLOAT(0.0f, r.heater.duty());
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_duty_is_the_pi_output_for_the_shaped_setpoint);
  RUN_TEST(test_shaped_setpoint_is_between_the_measurement_and_the_target);
  RUN_TEST(test_integration_is_gated_to_holds);
  RUN_TEST(test_safe_output_resets_pid_and_shaper);
  RUN_TEST(test_timed_ramp_reaches_the_pi_unshaped);
  RUN_TEST(test_door_open_ends_the_run_without_faulting);
  RUN_TEST(test_arms_and_disarms_the_supervisor_around_the_run);
  RUN_TEST(test_executor_fault_routes_into_the_supervisor);
  return UNITY_END();
}
