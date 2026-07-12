// native_control suite — the CYD's HeartbeatSender cadence (design.md §9).
// Frames land on the controller side, decoded back through MessageRouter, so the
// test asserts on real Heartbeat contents and emission timing.
#include <unity.h>

#include "frame_link.h"
#include "heartbeat_sender.h"
#include "helpers/fake_clock.h"
#include "helpers/pipe_transport.h"
#include "link_params.h"
#include "message_router.h"
#include "messages.h"
#include "oven.pb.h"

struct HbCapture : protocol::IMessageObserver {
  int count = 0;
  oven_Heartbeat last = oven_Heartbeat_init_default;
  void onHeartbeat(const oven_Heartbeat &h) override {
    ++count;
    last = h;
  }
};

// One test rig: sender on the CYD side, capture on the controller side.
struct Rig {
  LoopbackPipe pipe;
  FakeClock clk;
  HbCapture cap;
  protocol::MessageRouter ctrl_router;
  protocol::FrameLink ctrl_link;
  protocol::IMessageObserver sink;
  protocol::MessageRouter cyd_router;
  protocol::FrameLink cyd_link;
  protocol::HeartbeatSender hbs;

  Rig()
      : ctrl_router(cap), ctrl_link(pipe.b(), TF_SLAVE, ctrl_router), cyd_router(sink),
        cyd_link(pipe.a(), TF_MASTER, cyd_router), hbs(cyd_link, clk) {}

  void pump() { ctrl_link.poll(); }
};

void setUp(void) {}
void tearDown(void) {}

// service() emits the first heartbeat immediately, then once per period; seq
// increments and session/enable ride along.
void test_emits_on_period(void) {
  Rig r;
  r.hbs.setSession(5);
  r.hbs.setEnable(true);

  r.hbs.service(); // first tick sends immediately
  r.pump();
  TEST_ASSERT_EQUAL_INT(1, r.cap.count);
  TEST_ASSERT_EQUAL_UINT32(5, r.cap.last.session);
  TEST_ASSERT_EQUAL_UINT32(0, r.cap.last.seq);
  TEST_ASSERT_TRUE(r.cap.last.enable);

  r.clk.advance(protocol::kHeartbeatPeriodMs - 1);
  r.hbs.service(); // too soon
  r.pump();
  TEST_ASSERT_EQUAL_INT(1, r.cap.count);

  r.clk.advance(1); // now at the period
  r.hbs.service();
  r.pump();
  TEST_ASSERT_EQUAL_INT(2, r.cap.count);
  TEST_ASSERT_EQUAL_UINT32(1, r.cap.last.seq);
}

// The enable bit and millis stamp reflect current state at send time.
void test_enable_and_millis_reflected(void) {
  Rig r;
  r.hbs.setSession(9);
  r.hbs.setEnable(false);
  r.clk.advance(1234);

  r.hbs.sendNow();
  r.pump();
  TEST_ASSERT_EQUAL_INT(1, r.cap.count);
  TEST_ASSERT_FALSE(r.cap.last.enable);
  TEST_ASSERT_EQUAL_UINT32(1234, r.cap.last.millis);
  TEST_ASSERT_EQUAL_UINT32(9, r.cap.last.session);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_emits_on_period);
  RUN_TEST(test_enable_and_millis_reflected);
  return UNITY_END();
}
