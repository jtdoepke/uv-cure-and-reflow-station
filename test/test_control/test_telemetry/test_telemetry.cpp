// native_control suite — the controller -> CYD hot path (design.md §9, backlog A9).
//
// Two claims, both of which the bench caught the hard way. First, the controller emits
// Telemetry on the §9 cadence unconditionally, run or no run. Second — the reason this stream
// exists at all beyond the live graph — the CYD reads its *arrival* as the controller's
// liveness, because nothing else does: the handshake latches, so before this the CYD went on
// reporting a healthy link over an unplugged cable.
#include <unity.h>

#include "cyd_link.h"
#include "frame_link.h"
#include "helpers/fake_clock.h"
#include "helpers/flaky_transport.h"
#include "helpers/pipe_transport.h"
#include "link_params.h"
#include "message_router.h"
#include "messages.h"
#include "oven.pb.h"
#include "schema.h"
#include "telemetry_sender.h"

namespace {

// Counts Telemetry frames as the CYD's app layer would see them (CydLink forwards them on).
struct TelemetryCounter : protocol::IMessageObserver {
  int frames = 0;
  oven_Telemetry last = oven_Telemetry_init_default;
  void onTelemetry(const oven_Telemetry &t) override {
    ++frames;
    last = t;
  }
};

// Controller's TelemetrySender on one end, a real CydLink on the other.
struct Rig {
  LoopbackPipe pipe;
  FlakyTransport ctrl_tx; // the controller's TX: drop_tx = the wire comes out
  FakeClock clk;
  protocol::MessageRouter ctrl_router;
  protocol::FrameLink ctrl_link;
  protocol::TelemetrySender telemetry;
  protocol::MessageRouter cyd_router;
  protocol::FrameLink cyd_link;
  protocol::CydLink cyd;
  TelemetryCounter app;

  Rig()
      : ctrl_tx(pipe.b()), ctrl_router(), ctrl_link(ctrl_tx, TF_SLAVE, ctrl_router),
        telemetry(ctrl_link, clk), cyd_router(), cyd_link(pipe.a(), TF_MASTER, cyd_router),
        cyd(cyd_link, clk) {
    cyd_router.setObserver(cyd);
    cyd.setAppObserver(app);
  }

  // Advance time in slices, servicing the sender and draining into the CYD.
  void run(uint32_t ms, uint32_t step = 10) {
    for (uint32_t t = 0; t < ms; t += step) {
      clk.advance(step);
      telemetry.service();
      cyd_link.poll();
    }
  }
};

} // namespace

void setUp(void) {}
void tearDown(void) {}

// Fire-on-tick at the §9 rate, with no run and nothing asked of it.
void test_telemetry_sends_on_the_period_unprompted(void) {
  Rig r;
  r.telemetry.service(); // first call emits immediately
  r.cyd_link.poll();
  TEST_ASSERT_EQUAL_INT(1, r.app.frames);

  r.clk.advance(protocol::kTelemetryPeriodMs - 1);
  r.telemetry.service();
  r.cyd_link.poll();
  TEST_ASSERT_EQUAL_INT(1, r.app.frames); // too soon

  r.clk.advance(1);
  r.telemetry.service();
  r.cyd_link.poll();
  TEST_ASSERT_EQUAL_INT(2, r.app.frames);
}

// The sender owns session/seq/ctrl_millis; the caller owns everything else and it survives.
void test_sender_stamps_identity_and_preserves_payload(void) {
  Rig r;
  r.telemetry.setSession(0xABC);
  r.telemetry.state().heater_duty = 0.5F;
  r.telemetry.sendNow();
  r.cyd_link.poll();

  TEST_ASSERT_EQUAL_UINT32(0xABC, r.app.last.session);
  TEST_ASSERT_EQUAL_FLOAT(0.5F, r.app.last.heater_duty);
  TEST_ASSERT_EQUAL_UINT32(0, r.app.last.seq); // first frame

  r.telemetry.sendNow();
  r.cyd_link.poll();
  TEST_ASSERT_EQUAL_UINT32(1, r.app.last.seq); // monotonic
}

// Session 0 is IDLE telemetry — what a stateless controller reports from boot (§9).
void test_idle_telemetry_carries_no_session(void) {
  Rig r;
  r.telemetry.sendNow();
  r.cyd_link.poll();
  TEST_ASSERT_EQUAL_UINT32(0, r.app.last.session);
}

// Fail-closed: a CYD that has heard nothing does not claim a link.
void test_link_is_dead_before_any_telemetry(void) {
  Rig r;
  TEST_ASSERT_FALSE(r.cyd.linkAlive());
}

// Telemetry flowing keeps the link alive indefinitely.
void test_streaming_telemetry_keeps_the_link_alive(void) {
  Rig r;
  r.run(5000);
  TEST_ASSERT_TRUE(r.cyd.linkAlive());
}

// THE REGRESSION (A9): the wire comes out, the controller keeps talking into it, and the CYD
// must notice. Before this the handshake's latched matched() meant Home claimed "Link" forever.
void test_link_goes_dead_when_telemetry_stops(void) {
  Rig r;
  r.run(1000);
  TEST_ASSERT_TRUE(r.cyd.linkAlive());

  r.ctrl_tx.drop_tx = true; // unplugged
  r.run(protocol::kLinkTimeoutMs);
  TEST_ASSERT_FALSE(r.cyd.linkAlive());
}

// ...and it comes back on its own when the cable goes back in. No latch: this indicator tracks
// presence, not history (A4b's trip() is what makes a *fault* sticky).
void test_link_recovers_when_telemetry_resumes(void) {
  Rig r;
  r.run(1000);
  r.ctrl_tx.drop_tx = true;
  r.run(protocol::kLinkTimeoutMs);
  TEST_ASSERT_FALSE(r.cyd.linkAlive());

  r.ctrl_tx.drop_tx = false;
  r.run(protocol::kTelemetryPeriodMs * 2);
  TEST_ASSERT_TRUE(r.cyd.linkAlive());
}

// A single dropped frame must not blink the indicator: the timeout is several periods wide for
// exactly this reason (the same "miss 3-4 and act" logic as the command-timeout).
void test_one_dropped_frame_does_not_kill_the_link(void) {
  Rig r;
  r.run(1000);

  r.ctrl_tx.drop_tx = true;
  r.run(protocol::kTelemetryPeriodMs + 10); // lose ~one frame
  r.ctrl_tx.drop_tx = false;
  r.run(protocol::kTelemetryPeriodMs);
  TEST_ASSERT_TRUE(r.cyd.linkAlive());
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_telemetry_sends_on_the_period_unprompted);
  RUN_TEST(test_sender_stamps_identity_and_preserves_payload);
  RUN_TEST(test_idle_telemetry_carries_no_session);
  RUN_TEST(test_link_is_dead_before_any_telemetry);
  RUN_TEST(test_streaming_telemetry_keeps_the_link_alive);
  RUN_TEST(test_link_goes_dead_when_telemetry_stops);
  RUN_TEST(test_link_recovers_when_telemetry_resumes);
  RUN_TEST(test_one_dropped_frame_does_not_kill_the_link);
  return UNITY_END();
}
