// native_control suite — the A10 closed-loop bench simulator, end to end (backlog A10).
//
// test_bench_link proved "frames stop → the output cuts" but drove the heater with a fixed duty
// stub and no temperature feedback. This composes the SAME two-facade rig with the FULL run path —
// ProfileExecutor (A6) + HeaterControl PID (A5) + SafetySupervisor (A4) via ControllerRunPath —
// closed around the OvenPlant twin (A10). It exercises exactly what runs on the CONTROL_SIM
// controller firmware: a real Recipe/Start over the link arms a run, the executor sequences it, the
// PID tracks the setpoint against synthetic thermocouples, and the plant responds — with no oven.
//
// The headline is the backlog's acceptance example: drive a profile and watch it ramp → soak →
// peak → coast to touch-safe → report DONE. Plus the safety interactions the plant makes reachable:
// a welded SSR (HEATER_STUCK) and a lost sensor (SENSOR_FAULT).
#include <cmath>
#include <cstdio>
#include <unity.h>

#include "controller_link.h"
#include "cyd_link.h"
#include "frame_link.h"
#include "heater_actuator.h"
#include "heater_control.h"
#include "helpers/fake_clock.h"
#include "helpers/fake_contactor.h"
#include "helpers/fake_door_sensor.h"
#include "helpers/fake_heater_switch.h"
#include "helpers/pipe_transport.h"
#include "link_params.h"
#include "message_router.h"
#include "messages.h"
#include "oven.pb.h"
#include "oven_cal.h"
#include "oven_plant.h"
#include "oven_safety.h"
#include "profile_executor.h"
#include "run_path.h"
#include "safety_supervisor.h"
#include "sim_thermocouples.h"

namespace {

constexpr uint32_t kCydNonce = 0xC1D0A100;
constexpr uint32_t kCtrlNonce = 0xC710A100;
constexpr uint32_t kSession = 0xBE0CA100;

// The full CONTROL_SIM object graph: both link facades over one pipe, the run path, and the plant.
// The same classes src_control/main.cpp builds under CONTROL_SIM — only the injection site differs.
struct SimRig {
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
  OvenPlant plant;
  SimThermocouples tc;
  FakeDoorSensor door;
  SafetySupervisor safety;
  ControllerRunPath runpath;

  float weldDuty = 0.0f; // >0 simulates a welded SSR: the plant heats regardless of commanded duty
  uint32_t stepMs = 500; // sim slice; the plant integrates this interval each controller loop

  SimRig()
      : cyd_router(), cyd_link(pipe.a(), TF_MASTER, cyd_router), cyd(cyd_link, clk), ctrl_router(),
        ctrl_link(pipe.b(), TF_SLAVE, ctrl_router), ctrl(ctrl_link, clk), heater(heater_sw, clk),
        exec(clk), pid(clk), plant(), tc(plant), safety(ctrl, heater, contactor, tc, clk),
        runpath(exec, pid, safety, heater, ctrl, tc, door, oven_cal::kDefaultModel) {
    cyd_router.setObserver(cyd);
    ctrl_router.setObserver(ctrl);
    ctrl.setExecutor(exec); // link drives load/start/abort; the run path ticks it
  }

  // One controller loop, in main.cpp's CONTROL_SIM order.
  void controllerLoop() {
    ctrl_link.poll();
    ctrl.service();
    runpath.tick();
    heater.tick();
    safety.tick(); // LAST: safety has the final word
    const float applied = heater.duty() > weldDuty ? heater.duty() : weldDuty;
    const ProfileExecutor::Output &o = runpath.output();
    plant.step(static_cast<float>(stepMs) / 1000.0f, applied, o.convFan, o.uv, o.motor, door.open);
  }

  void cydLoop() {
    cyd_link.poll();
    cyd.service();
  }

  void exchange() {
    controllerLoop();
    cydLoop();
  }

  void run(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += stepMs) {
      clk.advance(stepMs);
      exchange();
    }
  }

  // Advance until `pred()` holds or `maxMs` elapses; returns whether the predicate was met.
  template <typename Pred> bool runUntil(Pred pred, uint32_t maxMs) {
    for (uint32_t t = 0; t < maxMs; t += stepMs) {
      clk.advance(stepMs);
      exchange();
      if (pred()) {
        return true;
      }
    }
    return false;
  }

  // Handshake, then upload + Start the recipe with enabled heartbeats — the CYD's run stimulus.
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

// A cure profile: warm to 80 °C (fan+UV+turntable), hold, then a compiled cool segment to
// touch-safe (the tail the CYD compiler appends, §5). uv/motor mark it MODE_CURE.
oven_Recipe cureRecipe() {
  oven_Recipe r = oven_Recipe_init_default;
  r.id = 11;
  r.mode = oven_Mode_MODE_CURE;
  r.segments_count = 3;
  r.segments[0] = seg(oven_Interp_INTERP_RAMP_OVER_TIME, 80.0f, 200000, true, true, true);
  r.segments[1] = seg(oven_Interp_INTERP_HOLD, 80.0f, 60000, true, true, true);
  r.segments[2] = seg(oven_Interp_INTERP_RAMP_OVER_TIME, 43.0f, 600000, false, false, false);
  return r;
}

// A reflow profile: ramp to 200 °C (convection on), soak, then a compiled cool segment. No
// uv/motor → MODE_REFLOW, so the executor gates hold-entry on the (lagging) workpiece.
oven_Recipe reflowRecipe() {
  oven_Recipe r = oven_Recipe_init_default;
  r.id = 22;
  r.mode = oven_Mode_MODE_REFLOW;
  r.segments_count = 3;
  r.segments[0] = seg(oven_Interp_INTERP_RAMP_OVER_TIME, 200.0f, 400000, true, false, false);
  r.segments[1] = seg(oven_Interp_INTERP_HOLD, 200.0f, 60000, true, false, false);
  r.segments[2] = seg(oven_Interp_INTERP_RAMP_OVER_TIME, 43.0f, 1800000, false, false, false);
  return r;
}

bool isDone(const SimRig &r) {
  return r.exec.state() == oven_RunState_RUN_STATE_DONE;
}

} // namespace

void setUp(void) {}
void tearDown(void) {}

// The acceptance run (cure): authorize, ramp to ~80 °C with UV on, then coast to touch-safe and
// report DONE — the whole vertical slice against the real firmware run path, no oven.
void test_cure_run_ramps_holds_and_finishes_touch_safe(void) {
  SimRig r;
  r.startRun(cureRecipe());

  // It reaches the hold near 80 °C with the cure channels on.
  const bool hot = r.runUntil([&] { return r.plant.wallTempC() >= 78.0f; }, 400000);
  TEST_ASSERT_TRUE(hot);
  TEST_ASSERT_TRUE(r.runpath.output().uv);        // UV on during the cure
  TEST_ASSERT_TRUE(r.runpath.output().motor);     // turntable on
  TEST_ASSERT_TRUE(r.plant.wallTempC() < 100.0f); // held near target, not runaway
  TEST_ASSERT_FALSE(r.safety.faulted());          // a healthy run never trips a fault

  // It coasts to touch-safe and reports DONE, heater off.
  const bool done = r.runUntil([&] { return isDone(r); }, 1500000);
  TEST_ASSERT_TRUE(done);
  TEST_ASSERT_TRUE(r.plant.chamberTempC() <= oven_safety::TOUCH_SAFE_C + 1.0f);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, r.heater.duty()); // output off at completion

  // The CYD ends the session (STOP / enable=false) once it sees DONE → the contactor opens and the
  // oven is fully safe. (While the session stays authorized the contactor legitimately stays
  // closed; the run is simply idle-hot-off, §4.)
  r.cyd.heartbeat().setEnable(false);
  r.run(protocol::kCommandTimeoutMs + 2 * protocol::kHeartbeatPeriodMs);
  TEST_ASSERT_TRUE(r.safety.safe());
  TEST_ASSERT_FALSE(r.contactor.closed);
}

// The reflow run ramps to a real solder-range peak (workpiece-gated), soaks, and finishes.
void test_reflow_run_reaches_peak_and_finishes(void) {
  SimRig r;
  r.startRun(reflowRecipe());

  // The workpiece (the reflow control sensor) climbs to the soak band.
  const bool peaked = r.runUntil([&] { return r.plant.workpieceTempC() >= 195.0f; }, 1800000);
  TEST_ASSERT_TRUE(peaked);

  const bool done = r.runUntil([&] { return isDone(r); }, 2600000);
  TEST_ASSERT_TRUE(done);
  TEST_ASSERT_TRUE(r.plant.chamberTempC() <= oven_safety::TOUCH_SAFE_C + 1.0f);
}

// A welded SSR: the plant keeps heating while the loop commands ~0 duty. The L3 stuck-heater check
// must trip and safe the outputs — a safety interaction only a closed loop can exercise.
void test_welded_ssr_trips_stuck_heater(void) {
  SimRig r;
  r.startRun(cureRecipe());
  TEST_ASSERT_TRUE(r.runUntil([&] { return r.plant.wallTempC() >= 78.0f; }, 400000));

  r.weldDuty = 1.0f; // the SSR welds on: full heat regardless of the commanded (holding) duty
  const bool tripped = r.runUntil([&] { return r.safety.faulted(); }, 200000);
  TEST_ASSERT_TRUE(tripped);
  TEST_ASSERT_EQUAL(oven_FaultCode_FAULT_HEATER_STUCK, r.safety.faultCode());
  TEST_ASSERT_TRUE(r.safety.safe());     // mains isolated
  TEST_ASSERT_FALSE(r.contactor.closed); // contactor open
}

// A lost control sensor mid-run: the executor refuses to run blind and the supervisor safes.
void test_sensor_fault_trips_when_control_channel_faults(void) {
  SimRig r;
  r.startRun(reflowRecipe());
  TEST_ASSERT_TRUE(r.runUntil([&] { return r.plant.workpieceTempC() >= 60.0f; }, 600000));

  r.tc.workpieceFault = true; // the reflow control probe opens
  const bool tripped = r.runUntil(
      [&] { return r.exec.state() == oven_RunState_RUN_STATE_FAULT || r.safety.faulted(); },
      120000);
  TEST_ASSERT_TRUE(tripped);
  TEST_ASSERT_TRUE(r.safety.safe());
}

// §15 (DECIDED): opening the door mid-run safes and ends the run to IDLE — and is NOT a fault.
// §22 is explicit that a door-open is an expected event, not a red alarm, so nothing here may
// latch: the operator gets "Run aborted — door opened", not a modal demanding an acknowledge.
void test_door_open_ends_run_to_idle_without_faulting(void) {
  SimRig r;
  r.startRun(cureRecipe());
  TEST_ASSERT_TRUE(r.runUntil([&] { return r.plant.wallTempC() >= 60.0f; }, 400000));
  TEST_ASSERT_EQUAL(oven_RunState_RUN_STATE_RUNNING, r.exec.state());

  r.door.open = true;
  const bool ended =
      r.runUntil([&] { return r.exec.state() != oven_RunState_RUN_STATE_RUNNING; }, 20000);

  TEST_ASSERT_TRUE(ended);
  TEST_ASSERT_EQUAL(oven_RunState_RUN_STATE_IDLE, r.exec.state()); // idle, not DONE, not FAULT
  TEST_ASSERT_FALSE(r.safety.faulted());                           // never latches (§22)
  TEST_ASSERT_EQUAL(oven_FaultCode_FAULT_NONE, r.safety.faultCode());
  TEST_ASSERT_EQUAL_FLOAT(0.0f, r.heater.duty()); // outputs commanded off the same tick
  TEST_ASSERT_TRUE(r.runpath.doorAborted());      // the edge is reported for the UI
  TEST_ASSERT_TRUE(r.runpath.doorOpen());
}

// The plant must model DS1, not merely report DS3: the interlock is in the element's LINE
// CONDUCTOR, so an open door removes heater power regardless of commanded duty. Without this the
// bench sim would model an oven that heats with its door open — and the door work would be tested
// against a fiction.
void test_door_open_kills_heat_even_with_duty_commanded(void) {
  SimRig r;
  r.startRun(cureRecipe());
  TEST_ASSERT_TRUE(r.runUntil([&] { return r.plant.wallTempC() >= 60.0f; }, 400000));

  // Weld the SSR full-on AND open the door. The commanded duty is irrelevant; DS1 wins.
  r.weldDuty = 1.0f;
  r.door.open = true;
  const float atOpen = r.plant.chamberTempC();
  r.run(120000);

  TEST_ASSERT_TRUE(r.plant.chamberTempC() < atOpen); // cooling, not heating, at full commanded duty
}

// Closing the door does NOT resume: §15 keeps the controller stateless — "no pause state, no resume
// logic, no context retained". Resume is the CYD re-Starting a generated remainder profile (B6).
void test_closing_door_does_not_resume(void) {
  SimRig r;
  r.startRun(cureRecipe());
  TEST_ASSERT_TRUE(r.runUntil([&] { return r.plant.wallTempC() >= 60.0f; }, 400000));

  r.door.open = true;
  TEST_ASSERT_TRUE(
      r.runUntil([&] { return r.exec.state() != oven_RunState_RUN_STATE_RUNNING; }, 20000));

  r.door.open = false;
  r.run(60000);
  TEST_ASSERT_EQUAL(oven_RunState_RUN_STATE_IDLE, r.exec.state()); // stays idle
  TEST_ASSERT_EQUAL_FLOAT(0.0f, r.heater.duty());
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_cure_run_ramps_holds_and_finishes_touch_safe);
  RUN_TEST(test_reflow_run_reaches_peak_and_finishes);
  RUN_TEST(test_welded_ssr_trips_stuck_heater);
  RUN_TEST(test_sensor_fault_trips_when_control_channel_faults);
  RUN_TEST(test_door_open_ends_run_to_idle_without_faulting);
  RUN_TEST(test_door_open_kills_heat_even_with_duty_commanded);
  RUN_TEST(test_closing_door_does_not_resume);
  return UNITY_END();
}
