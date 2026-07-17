// native_control suite — the controller -> CYD Fault annunciation (design.md §9/§22, A4b).
//
// The dedicated Fault frame the CYD's FaultController consumes. Two claims: it fires the
// moment the active code changes to a real fault (so the §22 modal is prompt), and because
// it is un-ACKed it re-sends on a cadence while the fault stays active, so a single dropped
// frame self-heals rather than losing the annunciation. Built like test_telemetry: a real
// FaultSender on one end, a real CydLink on the other, over a loopback that can drop a frame.
#include <unity.h>

#include "cyd_link.h"
#include "fault_sender.h"
#include "frame_link.h"
#include "helpers/fake_clock.h"
#include "helpers/flaky_transport.h"
#include "helpers/pipe_transport.h"
#include "link_params.h"
#include "message_router.h"
#include "messages.h"
#include "oven.pb.h"
#include "schema.h"

namespace {

// Counts Fault frames as the CYD's app layer would see them (CydLink forwards them on).
struct FaultCounter : protocol::IMessageObserver {
  int frames = 0;
  oven_Fault last = oven_Fault_init_default;
  void onFault(const oven_Fault &f) override {
    ++frames;
    last = f;
  }
};

// Controller's FaultSender on one end (behind a droppable TX), a real CydLink on the other.
struct Rig {
  LoopbackPipe pipe;
  FlakyTransport ctrl_tx; // the controller's TX: drop_tx swallows the wire bytes
  FakeClock clk;
  protocol::MessageRouter ctrl_router;
  protocol::FrameLink ctrl_link;
  protocol::FaultSender fault;
  protocol::MessageRouter cyd_router;
  protocol::FrameLink cyd_link;
  protocol::CydLink cyd;
  FaultCounter app;

  Rig()
      : ctrl_tx(pipe.b()), ctrl_router(), ctrl_link(ctrl_tx, TF_SLAVE, ctrl_router),
        fault(ctrl_link, clk), cyd_router(), cyd_link(pipe.a(), TF_MASTER, cyd_router),
        cyd(cyd_link, clk) {
    cyd_router.setObserver(cyd);
    cyd.setAppObserver(app);
  }

  void poll() { cyd_link.poll(); }

  // Advance time in slices, servicing the sender and draining into the CYD.
  void run(uint32_t ms, uint32_t step = 10) {
    for (uint32_t t = 0; t < ms; t += step) {
      clk.advance(step);
      fault.service();
      cyd_link.poll();
    }
  }
};

} // namespace

void setUp(void) {}
void tearDown(void) {}

// A change to a real code emits immediately, carrying session + code.
void test_fires_on_change(void) {
  Rig r;
  r.fault.setSession(0x1234);
  r.fault.set(oven_FaultCode_FAULT_OVERTEMP_CHAMBER);
  r.poll();
  TEST_ASSERT_EQUAL_INT(1, r.app.frames);
  TEST_ASSERT_EQUAL(oven_FaultCode_FAULT_OVERTEMP_CHAMBER, r.app.last.code);
  TEST_ASSERT_EQUAL_UINT32(0x1234, r.app.last.session);
}

// FAULT_NONE emits nothing, and re-setting the same code does not re-emit off-cadence.
void test_none_and_idempotent_set_are_silent(void) {
  Rig r;
  r.fault.set(oven_FaultCode_FAULT_NONE);
  r.poll();
  TEST_ASSERT_EQUAL_INT(0, r.app.frames);

  r.fault.set(oven_FaultCode_FAULT_HEATER_STUCK);
  r.poll();
  TEST_ASSERT_EQUAL_INT(1, r.app.frames);

  r.fault.set(oven_FaultCode_FAULT_HEATER_STUCK); // same code, no cadence elapsed
  r.poll();
  TEST_ASSERT_EQUAL_INT(1, r.app.frames);
}

// While a fault stays active the frame re-sends every kFaultResendMs; clearing to NONE stops it.
void test_resends_while_active_then_stops(void) {
  Rig r;
  r.fault.set(oven_FaultCode_FAULT_RUNTIME_EXCEEDED);
  r.poll();
  TEST_ASSERT_EQUAL_INT(1, r.app.frames);

  r.run(protocol::kFaultResendMs); // one resend interval
  TEST_ASSERT_EQUAL_INT(2, r.app.frames);

  r.run(protocol::kFaultResendMs);
  TEST_ASSERT_EQUAL_INT(3, r.app.frames);

  r.fault.set(oven_FaultCode_FAULT_NONE); // condition cleared: no more frames
  r.run(3 * protocol::kFaultResendMs);
  TEST_ASSERT_EQUAL_INT(3, r.app.frames);
}

// A dropped fire-on-change frame self-heals: the next cadence resend lands the annunciation.
void test_dropped_frame_self_heals(void) {
  Rig r;
  r.ctrl_tx.drop_tx = true; // swallow the immediate on-change frame
  r.fault.set(oven_FaultCode_FAULT_SENSOR_FAULT);
  r.poll();
  TEST_ASSERT_EQUAL_INT(0, r.app.frames); // lost

  r.ctrl_tx.drop_tx = false;
  r.run(protocol::kFaultResendMs); // the resend gets through
  TEST_ASSERT_EQUAL_INT(1, r.app.frames);
  TEST_ASSERT_EQUAL(oven_FaultCode_FAULT_SENSOR_FAULT, r.app.last.code);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_fires_on_change);
  RUN_TEST(test_none_and_idempotent_set_are_silent);
  RUN_TEST(test_resends_while_active_then_stops);
  RUN_TEST(test_dropped_frame_self_heals);
  return UNITY_END();
}
