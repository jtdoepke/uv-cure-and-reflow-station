// native_control suite — the controller's output safety gate (design.md §4, A4a).
//
// Drives run authorization through a real ControllerLink (handshake + session gate)
// and asserts SafetySupervisor's effect on a FakeContactor + a HeaterActuator over a
// FakeHeaterSwitch. The command-timeout case is the unit-level form of the §8 step-1
// fail-safe proof (backlog A8): stop the heartbeats, the outputs cut within 750 ms.
#include <unity.h>

#include "IContactor.h"
#include "IWatchdog.h"
#include "controller_link.h"
#include "frame_link.h"
#include "handshake.h"
#include "heater_actuator.h"
#include "helpers/fake_clock.h"
#include "helpers/fake_contactor.h"
#include "helpers/fake_heater_switch.h"
#include "helpers/fake_thermocouples.h"
#include "helpers/pipe_transport.h"
#include "link_params.h"
#include "message_router.h"
#include "messages.h"
#include "oven.pb.h"
#include "oven_safety.h"
#include "safety_supervisor.h"
#include "schema.h"

// A ControllerLink needs a FrameLink; the send side is unused here (we drive the
// gate and handshake directly, as test_session_gate does). The heater is a real
// HeaterActuator so "not forced off" is observable as a live switch line.
struct SupervisorFixture {
  LoopbackPipe pipe;
  protocol::IMessageObserver sink;
  protocol::MessageRouter router;
  FakeClock clk;
  protocol::FrameLink link;
  ControllerLink ctrl;
  FakeHeaterSwitch heaterSw;
  FakeContactor contactor;
  FakeThermocouples tc;
  HeaterActuator heater;
  SafetySupervisor sup;

  SupervisorFixture()
      : router(sink), link(pipe.a(), TF_MASTER, router), ctrl(link, clk), heater(heaterSw, clk),
        sup(ctrl, heater, contactor, tc, clk) {}

  void makeMatched() {
    ctrl.handshake().onPeerHello(hello(protocol::kProtoVer, protocol::kSchemaHash));
  }

  // Bring the gate to a fully authorized state: matched, session adopted, one fresh
  // enabled heartbeat.
  void authorize(uint32_t session) {
    makeMatched();
    ctrl.gate().adoptSession(session);
    ctrl.gate().onHeartbeat(hb(session, 0, true));
  }

  // Drive the heater switch fully on via the actuator, so a later forceOff is visible.
  void driveHeaterOn() {
    heater.setDuty(1.0F);
    heater.tick();
  }

  // Re-stamp a fresh enabled heartbeat so authorization survives a clock advance longer
  // than the command timeout (the L3 bounds — runtime, stuck-window — outlast 750 ms).
  void refresh(uint32_t session, uint32_t seq) { ctrl.gate().onHeartbeat(hb(session, seq, true)); }

  // Arm the supervisor's L3 checks against a one-segment recipe. `cure` sets uv on the
  // segment, which forces the cure hard-max via deriveMode (content, not tag); otherwise it
  // is a plain-heat reflow recipe. `durMs` seeds the runtime budget (× RUNTIME_MARGIN_FRAC).
  void armSup(bool cure, uint32_t durMs) {
    oven_Recipe r = oven_Recipe_init_zero;
    r.segments_count = 1;
    r.segments[0].heat_c = cure ? 80.0F : 200.0F;
    r.segments[0].dur_ms = durMs;
    r.segments[0].uv = cure;
    sup.armRun(r);
  }

  static oven_Hello hello(uint32_t proto_ver, uint64_t schema_hash) {
    oven_Hello h = oven_Hello_init_default;
    h.proto_ver = proto_ver;
    h.schema_hash = schema_hash;
    return h;
  }

  static oven_Heartbeat hb(uint32_t session, uint32_t seq, bool enable) {
    oven_Heartbeat h = oven_Heartbeat_init_default;
    h.session = session;
    h.seq = seq;
    h.enable = enable;
    return h;
  }
};

void setUp(void) {}
void tearDown(void) {}

// Construction alone establishes the fail-safe default: contactor open, heater off,
// before any tick() or traffic.
void test_boot_is_fail_safe(void) {
  SupervisorFixture f;
  TEST_ASSERT_FALSE(f.contactor.closed);
  TEST_ASSERT_FALSE(f.heaterSw.on);
  TEST_ASSERT_TRUE(f.sup.safe());
}

// A fully authorized run closes the contactor and leaves the heater alone (the
// supervisor never forces off while authorized — the PID owns the duty).
void test_authorized_run_closes_contactor_and_keeps_heater(void) {
  SupervisorFixture f;
  f.authorize(42);
  f.driveHeaterOn();
  TEST_ASSERT_TRUE(f.heaterSw.on); // actuator drove it on
  f.sup.tick();
  TEST_ASSERT_TRUE(f.contactor.closed);
  TEST_ASSERT_TRUE(f.heaterSw.on); // not cut
  TEST_ASSERT_FALSE(f.sup.safe());
}

// enable=false (a deliberate stop) forces the heater off and opens the contactor.
void test_enable_false_cuts_outputs(void) {
  SupervisorFixture f;
  f.makeMatched();
  f.ctrl.gate().adoptSession(1);
  f.ctrl.gate().onHeartbeat(SupervisorFixture::hb(1, 0, false));
  f.driveHeaterOn();
  f.sup.tick();
  TEST_ASSERT_FALSE(f.contactor.closed);
  TEST_ASSERT_FALSE(f.heaterSw.on);
}

// The A8 fail-safe proof in unit form: with the run authorized, heartbeats stop; the
// outputs stay live right up to the command-timeout, then cut on the tick past it.
void test_command_timeout_cuts_within_budget(void) {
  SupervisorFixture f;
  f.authorize(7);
  f.driveHeaterOn();

  // Just inside the timeout: still authorized, outputs live.
  f.clk.advance(protocol::kCommandTimeoutMs - 1);
  f.sup.tick();
  TEST_ASSERT_TRUE(f.contactor.closed);
  TEST_ASSERT_TRUE(f.heaterSw.on);

  // Crossing the timeout: link is stale -> outputs cut.
  f.clk.advance(1);
  f.sup.tick();
  TEST_ASSERT_FALSE(f.contactor.closed);
  TEST_ASSERT_FALSE(f.heaterSw.on);
}

// Ending the run (Abort / session cleared) safes the oven.
void test_session_cleared_cuts_outputs(void) {
  SupervisorFixture f;
  f.authorize(3);
  f.driveHeaterOn();
  f.sup.tick();
  TEST_ASSERT_TRUE(f.contactor.closed);

  f.ctrl.gate().clearSession();
  f.sup.tick();
  TEST_ASSERT_FALSE(f.contactor.closed);
  TEST_ASSERT_FALSE(f.heaterSw.on);
}

// trip() latches the safe state even while otherwise authorized; it holds across
// ticks until clearFault(), after which a still-authorized run re-closes.
void test_trip_latches_until_cleared(void) {
  SupervisorFixture f;
  f.authorize(5);
  f.driveHeaterOn();
  f.sup.tick();
  TEST_ASSERT_TRUE(f.contactor.closed);

  f.sup.trip();
  TEST_ASSERT_TRUE(f.sup.faulted());
  TEST_ASSERT_FALSE(f.contactor.closed);
  TEST_ASSERT_FALSE(f.heaterSw.on);

  // Still authorized, but the latch keeps it safe across ticks.
  f.sup.tick();
  TEST_ASSERT_FALSE(f.contactor.closed);

  // Clearing the latch lets the (still fresh) authorization re-close on the next tick.
  f.sup.clearFault();
  f.sup.tick();
  TEST_ASSERT_TRUE(f.contactor.closed);
}

// trip(code) records the code faultCode() reports; the no-arg overload keeps A4a's latch
// behavior and reports the INTERNAL catch-all.
void test_trip_records_code(void) {
  SupervisorFixture f;
  f.sup.trip(oven_FaultCode_FAULT_OVERTEMP_CASE);
  TEST_ASSERT_EQUAL(oven_FaultCode_FAULT_OVERTEMP_CASE, f.sup.faultCode());
  f.sup.clearFault();
  TEST_ASSERT_EQUAL(oven_FaultCode_FAULT_NONE, f.sup.faultCode());
  f.sup.trip();
  TEST_ASSERT_EQUAL(oven_FaultCode_FAULT_INTERNAL, f.sup.faultCode());
}

// --- L3 clamps (A4b) --------------------------------------------------------------

// A measured high-limit reading above hardMax[mode] + margin opens the contactor and
// reports OVERTEMP_CHAMBER — even though the run is otherwise authorized.
void test_overtemp_trip_opens_contactor(void) {
  SupervisorFixture f;
  f.authorize(11);
  f.armSup(/*cure=*/true, /*durMs=*/600000); // cure hard-max 120 -> trip above 135
  f.tc.setAll(oven_safety::CURE_HARD_MAX_C + oven_safety::OVERTEMP_MARGIN_C + 5.0F);
  f.sup.tick();
  TEST_ASSERT_TRUE(f.sup.faulted());
  TEST_ASSERT_EQUAL(oven_FaultCode_FAULT_OVERTEMP_CHAMBER, f.sup.faultCode());
  TEST_ASSERT_TRUE(f.sup.safe());
  TEST_ASSERT_FALSE(f.contactor.closed);
}

// Just below the trip point, the run continues (contactor stays closed).
void test_overtemp_not_tripped_below_threshold(void) {
  SupervisorFixture f;
  f.authorize(12);
  f.armSup(/*cure=*/true, /*durMs=*/600000);
  f.tc.setAll(oven_safety::CURE_HARD_MAX_C + oven_safety::OVERTEMP_MARGIN_C - 5.0F);
  f.sup.tick();
  TEST_ASSERT_FALSE(f.sup.faulted());
  TEST_ASSERT_TRUE(f.contactor.closed);
}

// Temp rising past the threshold while commanded duty is ~0 across a full window is a
// welded SSR: trip HEATER_STUCK. Reflow mode keeps the over-temp trip (315) out of the way.
void test_stuck_heater_trips_on_rise_at_zero_duty(void) {
  SupervisorFixture f;
  f.authorize(13);
  f.tc.setAll(25.0F);
  f.armSup(/*cure=*/false, /*durMs=*/600000); // baseline captured at 25 C, duty stays 0
  f.tc.setAll(25.0F + oven_safety::STUCK_HEATER_RISE_C + 2.0F);
  f.clk.advance(oven_safety::STUCK_HEATER_WINDOW_MS);
  f.refresh(13, 1); // keep authorization fresh past the command timeout
  f.sup.tick();
  TEST_ASSERT_TRUE(f.sup.faulted());
  TEST_ASSERT_EQUAL(oven_FaultCode_FAULT_HEATER_STUCK, f.sup.faultCode());
  TEST_ASSERT_FALSE(f.contactor.closed);
}

// The same rise while the heater is legitimately commanded on is NOT stuck — the window
// re-anchors and no fault is raised.
void test_stuck_heater_not_tripped_while_heating(void) {
  SupervisorFixture f;
  f.authorize(14);
  f.tc.setAll(25.0F);
  f.armSup(/*cure=*/false, /*durMs=*/600000);
  f.driveHeaterOn(); // duty 1.0 -> rise is expected
  f.tc.setAll(25.0F + oven_safety::STUCK_HEATER_RISE_C + 2.0F);
  f.clk.advance(oven_safety::STUCK_HEATER_WINDOW_MS);
  f.refresh(14, 1);
  f.sup.tick();
  TEST_ASSERT_FALSE(f.sup.faulted());
  TEST_ASSERT_TRUE(f.contactor.closed);
}

// A run that outlives Σ(dur) × margin faults with RUNTIME_EXCEEDED.
void test_runtime_bound_trips(void) {
  SupervisorFixture f;
  f.authorize(15);
  f.tc.setAll(25.0F);
  f.armSup(/*cure=*/false, /*durMs=*/1000); // budget = 1000 * 1.5 = 1500 ms
  f.clk.advance(2000);
  f.refresh(15, 1);
  f.sup.tick();
  TEST_ASSERT_TRUE(f.sup.faulted());
  TEST_ASSERT_EQUAL(oven_FaultCode_FAULT_RUNTIME_EXCEEDED, f.sup.faultCode());
  TEST_ASSERT_FALSE(f.contactor.closed);
}

// No usable high-limit channel while running is itself a stop: refuse to run blind.
void test_all_high_limit_channels_faulted_trips_sensor_fault(void) {
  SupervisorFixture f;
  f.authorize(16);
  f.armSup(/*cure=*/true, /*durMs=*/600000);
  for (int i = 0; i < FakeThermocouples::kWalls; ++i) {
    f.tc.wallFault[i] = true;
  }
  f.sup.tick();
  TEST_ASSERT_TRUE(f.sup.faulted());
  TEST_ASSERT_EQUAL(oven_FaultCode_FAULT_SENSOR_FAULT, f.sup.faultCode());
  TEST_ASSERT_FALSE(f.contactor.closed);
}

// disarmRun() releases every L3 check: an over-temp reading no longer trips (the run is
// over, so its temperatures are moot). Verifies the arm gate, not a safe-to-run claim.
void test_disarm_releases_l3(void) {
  SupervisorFixture f;
  f.authorize(17);
  f.armSup(/*cure=*/true, /*durMs=*/600000);
  f.tc.setAll(oven_safety::CURE_HARD_MAX_C + oven_safety::OVERTEMP_MARGIN_C + 20.0F);
  f.sup.disarmRun();
  f.sup.tick();
  TEST_ASSERT_FALSE(f.sup.faulted());
  TEST_ASSERT_TRUE(f.contactor.closed);
}

// clampSetpoint is total and caps to the armed mode's hard-max (or reflow when unarmed).
void test_clamp_setpoint_totality_and_ceiling(void) {
  SupervisorFixture f;
  // Unarmed: ceiling is the reflow hard-max.
  TEST_ASSERT_EQUAL_FLOAT(oven_safety::REFLOW_HARD_MAX_C, f.sup.clampSetpoint(500.0F));
  TEST_ASSERT_EQUAL_FLOAT(0.0F, f.sup.clampSetpoint(-5.0F));
  TEST_ASSERT_EQUAL_FLOAT(0.0F, f.sup.clampSetpoint(NAN));
  TEST_ASSERT_EQUAL_FLOAT(oven_safety::REFLOW_HARD_MAX_C, f.sup.clampSetpoint(INFINITY));
  TEST_ASSERT_EQUAL_FLOAT(50.0F, f.sup.clampSetpoint(50.0F));
  // Armed cure: ceiling tightens to the cure hard-max.
  f.armSup(/*cure=*/true, /*durMs=*/600000);
  TEST_ASSERT_EQUAL_FLOAT(oven_safety::CURE_HARD_MAX_C, f.sup.clampSetpoint(500.0F));
  TEST_ASSERT_EQUAL_FLOAT(80.0F, f.sup.clampSetpoint(80.0F));
}

// A watchdog reset from the previous boot is reported once (annunciation, not a latch);
// an ordinary boot reports nothing.
void test_watchdog_reset_reports_fault(void) {
  SupervisorFixture f;
  f.sup.noteResetCause(ResetCause::Watchdog);
  TEST_ASSERT_EQUAL(oven_FaultCode_FAULT_WATCHDOG, f.sup.faultCode());
  TEST_ASSERT_FALSE(f.sup.faulted()); // not sticky: the controller is already safe

  SupervisorFixture g;
  g.sup.noteResetCause(ResetCause::PowerOn);
  TEST_ASSERT_EQUAL(oven_FaultCode_FAULT_NONE, g.sup.faultCode());
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_boot_is_fail_safe);
  RUN_TEST(test_authorized_run_closes_contactor_and_keeps_heater);
  RUN_TEST(test_enable_false_cuts_outputs);
  RUN_TEST(test_command_timeout_cuts_within_budget);
  RUN_TEST(test_session_cleared_cuts_outputs);
  RUN_TEST(test_trip_latches_until_cleared);
  RUN_TEST(test_trip_records_code);
  RUN_TEST(test_overtemp_trip_opens_contactor);
  RUN_TEST(test_overtemp_not_tripped_below_threshold);
  RUN_TEST(test_stuck_heater_trips_on_rise_at_zero_duty);
  RUN_TEST(test_stuck_heater_not_tripped_while_heating);
  RUN_TEST(test_runtime_bound_trips);
  RUN_TEST(test_all_high_limit_channels_faulted_trips_sensor_fault);
  RUN_TEST(test_disarm_releases_l3);
  RUN_TEST(test_clamp_setpoint_totality_and_ceiling);
  RUN_TEST(test_watchdog_reset_reports_fault);
  return UNITY_END();
}
