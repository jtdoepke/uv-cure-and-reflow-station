// native_control suite — the §8 step-1 fail-safe proof, in software (backlog A8).
//
// This is the join the other suites leave open. test_safety_supervisor reaches into
// ControllerLink::gate() and drives adoptSession()/onHeartbeat() by hand, so it proves the
// supervisor's policy but never sends a frame; test_reliability_integration sends the real
// frames but stops at ctrl.authorized() and owns no outputs. Neither asserts the thing the
// bench actually demonstrates: real frames stop arriving, therefore the heater OUTPUT goes off.
//
// So this composes test_reliability_integration's two-facade Rig with A4a's output stack and
// asserts end to end, with FlakyTransport::drop_tx standing in for the pulled CYD TX wire. It
// mirrors the hardware procedure step for step, so the bench run confirms rather than discovers.
#include <unity.h>

#include "controller_link.h"
#include "cyd_link.h"
#include "frame_link.h"
#include "heater_actuator.h"
#include "helpers/fake_clock.h"
#include "helpers/fake_contactor.h"
#include "helpers/fake_thermocouples.h"
#include "helpers/fake_heater_switch.h"
#include "helpers/flaky_transport.h"
#include "helpers/pipe_transport.h"
#include "link_params.h"
#include "message_router.h"
#include "messages.h"
#include "oven.pb.h"
#include "safety_supervisor.h"
#include "schema.h"

using protocol::ReliableSender;

namespace {

// Distinct per-board boot nonces (§9 re-sync); on hardware these are re-rolled each boot.
constexpr uint32_t kCydNonce = 0xC1D00001;
constexpr uint32_t kCtrlNonce = 0xC7100001;
constexpr uint32_t kSession = 0xBE0C0001;
// What the bench's duty stub commands while authorized. At the actuator's default 1 s window
// this is the 500 ms blink; here it just has to be something the supervisor can be seen to cut.
constexpr float kBenchDuty = 0.5F;

// The full bench object graph: both link facades over one pipe, plus the controller's outputs.
// The same classes main.cpp builds — only the injection site differs (§11).
struct BenchRig {
  LoopbackPipe pipe;
  FlakyTransport cyd_tx; // wraps the CYD's TX: drop_tx is the pulled wire
  FakeClock clk;
  protocol::MessageRouter cyd_router;
  protocol::FrameLink cyd_link;
  protocol::CydLink cyd;
  protocol::MessageRouter ctrl_router;
  protocol::FrameLink ctrl_link;
  ControllerLink ctrl;
  FakeHeaterSwitch heater_sw;
  FakeContactor contactor;
  FakeThermocouples tc;
  HeaterActuator heater;
  SafetySupervisor safety;

  BenchRig()
      : cyd_tx(pipe.a()), cyd_router(), cyd_link(cyd_tx, TF_MASTER, cyd_router), cyd(cyd_link, clk),
        ctrl_router(), ctrl_link(pipe.b(), TF_SLAVE, ctrl_router), ctrl(ctrl_link, clk),
        heater(heater_sw, clk), safety(ctrl, heater, contactor, tc, clk) {
    cyd_router.setObserver(cyd);
    ctrl_router.setObserver(ctrl);
  }

  // One controller loop, in main.cpp's order: drain the wire, service the link, let the bench
  // duty stub command heat, tick the actuator, then let safety have the last word.
  void controllerLoop() {
    ctrl_link.poll();
    ctrl.service();
    heater.setDuty(ctrl.authorized() ? kBenchDuty : 0.0F);
    heater.tick();
    safety.tick();
  }

  void cydLoop() {
    cyd_link.poll();
    cyd.service();
  }

  void exchange() {
    controllerLoop();
    cydLoop();
  }

  // Run both loops across `ms` of simulated time in `step` slices.
  void run(uint32_t ms, uint32_t step = 10) {
    for (uint32_t t = 0; t < ms; t += step) {
      clk.advance(step);
      exchange();
    }
  }

  // Boot -> handshake -> recipe -> start -> enabled heartbeats, i.e. the CYD bench stimulus.
  void bringUpAuthorizedRun() {
    cyd.begin(kCydNonce);
    ctrl.begin(kCtrlNonce);
    exchange();

    oven_Recipe rec = oven_Recipe_init_default;
    rec.id = 1;
    rec.mode = oven_Mode_MODE_CURE;
    rec.segments_count = 1;
    rec.segments[0].dur_ms = 60000;
    rec.segments[0].heat_c = 80.0F;
    rec.segments[0].interp = oven_Interp_INTERP_HOLD;
    cyd.sender().sendRecipe(rec);
    exchange();

    oven_Start st = oven_Start_init_default;
    st.session = kSession;
    st.recipe_id = 1;
    cyd.sender().sendStart(st);
    exchange();

    cyd.heartbeat().setSession(kSession);
    cyd.heartbeat().setEnable(true);
  }
};

} // namespace

void setUp(void) {}
void tearDown(void) {}

// Before any traffic at all: mains isolated, heat never commanded. §8 step 1's "outputs default
// OFF" — the state the controller must hold with no CYD present, indefinitely.
void test_boot_defaults_are_safe(void) {
  BenchRig r;
  TEST_ASSERT_TRUE(r.safety.safe());
  TEST_ASSERT_FALSE(r.contactor.closed);

  r.run(2000); // no CYD, no frames
  TEST_ASSERT_TRUE(r.safety.safe());
  TEST_ASSERT_FALSE(r.contactor.closed);
  TEST_ASSERT_FALSE(r.heater_sw.on);
  TEST_ASSERT_FALSE(r.ctrl.authorized());
}

// The full authorization chain over real frames: handshake, recipe, start, enabled heartbeats ->
// the contactor closes and the heater output is actually driven.
void test_authorized_run_drives_the_outputs(void) {
  BenchRig r;
  r.bringUpAuthorizedRun();
  r.run(1000);

  TEST_ASSERT_TRUE(r.ctrl.authorized());
  TEST_ASSERT_TRUE(r.contactor.closed);
  TEST_ASSERT_FALSE(r.safety.safe());            // mains present: a run is actively commanded
  TEST_ASSERT_TRUE(r.heater_sw.transitions > 0); // the LED "heater" is being switched
}

// THE PROOF (§8 step 1): pull the CYD's TX and the heater output dies within the command
// timeout, with the contactor open behind it. Silence reads as "stop", never as consent.
void test_pulled_tx_cuts_the_output_within_the_timeout(void) {
  BenchRig r;
  r.bringUpAuthorizedRun();
  r.run(1000);
  TEST_ASSERT_TRUE(r.contactor.closed); // running before the cut

  r.cyd_tx.drop_tx = true; // the wire comes off; the CYD keeps talking into it
  r.run(protocol::kCommandTimeoutMs + protocol::kHeartbeatPeriodMs);

  TEST_ASSERT_FALSE(r.ctrl.authorized());
  TEST_ASSERT_FALSE(r.heater_sw.on);
  TEST_ASSERT_FALSE(r.contactor.closed);
  TEST_ASSERT_TRUE(r.safety.safe());
}

// enable=false is the explicit stop, and must not wait for any timeout: the operator pressing
// STOP is not the same event as a wire falling out.
void test_enable_low_cuts_the_output_immediately(void) {
  BenchRig r;
  r.bringUpAuthorizedRun();
  r.run(1000);
  TEST_ASSERT_TRUE(r.contactor.closed);

  r.cyd.heartbeat().setEnable(false);
  r.run(protocol::kHeartbeatPeriodMs * 2); // one fresh heartbeat carrying enable=false

  TEST_ASSERT_FALSE(r.ctrl.authorized());
  TEST_ASSERT_FALSE(r.heater_sw.on);
  TEST_ASSERT_TRUE(r.safety.safe());
}

// A8 proves the CUT, not a latch: restoring the wire re-authorizes on its own, because the
// session is still active and the heartbeats are fresh again. A4b's trip() is what makes a
// fault sticky. This is expected to read as a bug; it is not.
void test_restored_link_reauthorizes(void) {
  BenchRig r;
  r.bringUpAuthorizedRun();
  r.run(1000);

  r.cyd_tx.drop_tx = true;
  r.run(protocol::kCommandTimeoutMs + protocol::kHeartbeatPeriodMs);
  TEST_ASSERT_TRUE(r.safety.safe());

  r.cyd_tx.drop_tx = false; // wire back on
  r.run(protocol::kHeartbeatPeriodMs * 2);
  TEST_ASSERT_TRUE(r.ctrl.authorized());
  TEST_ASSERT_TRUE(r.contactor.closed);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_boot_defaults_are_safe);
  RUN_TEST(test_authorized_run_drives_the_outputs);
  RUN_TEST(test_pulled_tx_cuts_the_output_within_the_timeout);
  RUN_TEST(test_enable_low_cuts_the_output_immediately);
  RUN_TEST(test_restored_link_reauthorizes);
  return UNITY_END();
}
