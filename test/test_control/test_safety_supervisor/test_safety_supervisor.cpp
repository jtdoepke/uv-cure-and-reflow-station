// native_control suite — the controller's output safety gate (design.md §4, A4a).
//
// Drives run authorization through a real ControllerLink (handshake + session gate)
// and asserts SafetySupervisor's effect on a FakeContactor + a HeaterActuator over a
// FakeHeaterSwitch. The command-timeout case is the unit-level form of the §8 step-1
// fail-safe proof (backlog A8): stop the heartbeats, the outputs cut within 750 ms.
#include <unity.h>

#include "IContactor.h"
#include "controller_link.h"
#include "frame_link.h"
#include "handshake.h"
#include "heater_actuator.h"
#include "helpers/fake_clock.h"
#include "helpers/fake_contactor.h"
#include "helpers/fake_heater_switch.h"
#include "helpers/pipe_transport.h"
#include "link_params.h"
#include "message_router.h"
#include "messages.h"
#include "oven.pb.h"
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
  HeaterActuator heater;
  SafetySupervisor sup;

  SupervisorFixture()
      : router(sink), link(pipe.a(), TF_MASTER, router), ctrl(link, clk), heater(heaterSw, clk),
        sup(ctrl, heater, contactor) {}

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

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_boot_is_fail_safe);
  RUN_TEST(test_authorized_run_closes_contactor_and_keeps_heater);
  RUN_TEST(test_enable_false_cuts_outputs);
  RUN_TEST(test_command_timeout_cuts_within_budget);
  RUN_TEST(test_session_cleared_cuts_outputs);
  RUN_TEST(test_trip_latches_until_cleared);
  return UNITY_END();
}
