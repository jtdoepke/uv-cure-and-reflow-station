// native_control suite — the full A2 reliability layer end to end (design.md §9).
// Two facades (CydLink + ControllerLink) over one LoopbackPipe walk the whole
// boot-to-run path: Hello handshake -> Recipe upload -> Start -> heartbeats
// authorize -> heartbeats stop -> authorization drops at the command-timeout.
#include <unity.h>

#include "controller_link.h"
#include "cyd_link.h"
#include "frame_link.h"
#include "helpers/fake_clock.h"
#include "helpers/pipe_transport.h"
#include "link_params.h"
#include "message_router.h"
#include "messages.h"
#include "oven.pb.h"
#include "schema.h"

using protocol::ReliableSender;

struct Rig {
  LoopbackPipe pipe;
  FakeClock clk;
  protocol::MessageRouter cyd_router;
  protocol::FrameLink cyd_link;
  protocol::CydLink cyd;
  protocol::MessageRouter ctrl_router;
  protocol::FrameLink ctrl_link;
  ControllerLink ctrl;

  Rig()
      : cyd_router(), cyd_link(pipe.a(), TF_MASTER, cyd_router), cyd(cyd_link, clk), ctrl_router(),
        ctrl_link(pipe.b(), TF_SLAVE, ctrl_router), ctrl(ctrl_link, clk) {
    cyd_router.setObserver(cyd);
    ctrl_router.setObserver(ctrl);
  }

  // One drain in each direction: controller consumes then replies, CYD consumes.
  void exchange() {
    ctrl_link.poll();
    cyd_link.poll();
  }
};

// Distinct per-board boot nonces (§9 re-sync). Arbitrary here; on hardware they are
// re-rolled every boot, which is what lets a peer notice a restart.
static constexpr uint32_t kCydNonce = 0xC1DB0071;
static constexpr uint32_t kCtrlNonce = 0xC7180071;

static oven_Recipe cureRecipe(uint32_t id) {
  oven_Recipe rec = oven_Recipe_init_default;
  rec.id = id;
  rec.mode = oven_Mode_MODE_CURE;
  rec.segments_count = 1;
  rec.segments[0].dur_ms = 1000;
  rec.segments[0].heat_c = 80.0F;
  rec.segments[0].uv = true;
  rec.segments[0].interp = oven_Interp_INTERP_HOLD;
  return rec;
}

void setUp(void) {}
void tearDown(void) {}

void test_full_boot_to_run_and_timeout(void) {
  Rig r;
  const uint32_t kSession = 0xABCDE;

  // --- boot handshake ---
  r.cyd.begin(kCydNonce);
  r.ctrl.begin(kCtrlNonce);
  r.exchange();
  TEST_ASSERT_TRUE(r.cyd.handshake().matched());
  TEST_ASSERT_TRUE(r.ctrl.handshake().matched());
  TEST_ASSERT_FALSE(r.ctrl.authorized()); // no session/heartbeat yet

  // --- recipe upload (setup path) ---
  TEST_ASSERT_TRUE(r.cyd.sender().sendRecipe(cureRecipe(1)));
  r.exchange();
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ReliableSender::State::Acked),
                        static_cast<int>(r.cyd.sender().state()));

  // --- start (setup path) — adopts the session on the controller ---
  r.cyd.heartbeat().setSession(kSession);
  r.cyd.heartbeat().setEnable(true);
  oven_Start st = oven_Start_init_default;
  st.session = kSession;
  st.recipe_id = 1;
  TEST_ASSERT_TRUE(r.cyd.sender().sendStart(st));
  r.exchange();
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ReliableSender::State::Acked),
                        static_cast<int>(r.cyd.sender().state()));
  TEST_ASSERT_TRUE(r.ctrl.gate().hasActiveSession());
  TEST_ASSERT_EQUAL_UINT32(kSession, r.ctrl.gate().activeSession());

  // --- heartbeats authorize the run ---
  for (int i = 0; i < 3; ++i) {
    r.cyd.service(); // emits a heartbeat on the period boundary
    r.exchange();
    TEST_ASSERT_TRUE(r.ctrl.authorized());
    r.clk.advance(protocol::kHeartbeatPeriodMs);
  }

  // --- heartbeats stop -> authorization drops at the command-timeout ---
  r.clk.advance(protocol::kCommandTimeoutMs);
  TEST_ASSERT_FALSE(r.ctrl.authorized());
}

// Losing the schema match mid-run (a controller-rebuild skew) de-authorizes even
// with a fresh, enabled heartbeat — the fail-closed gate.
void test_schema_mismatch_blocks_run(void) {
  Rig r;
  const uint32_t kSession = 7;

  r.cyd.begin(kCydNonce);
  r.ctrl.begin(kCtrlNonce);
  r.exchange();

  // Bring a session up and authorize it.
  oven_Recipe rec = cureRecipe(2);
  r.cyd.sender().sendRecipe(rec);
  r.exchange();
  r.cyd.heartbeat().setSession(kSession);
  r.cyd.heartbeat().setEnable(true);
  oven_Start st = oven_Start_init_default;
  st.session = kSession;
  st.recipe_id = 2;
  r.cyd.sender().sendStart(st);
  r.exchange();
  r.cyd.service();
  r.exchange();
  TEST_ASSERT_TRUE(r.ctrl.authorized());

  // A peer Hello built against a different .proto arrives -> match drops ->
  // authorization is refused despite a fresh enabled heartbeat.
  oven_Hello skewed = oven_Hello_init_default;
  skewed.proto_ver = protocol::kProtoVer;
  skewed.schema_hash = protocol::kSchemaHash ^ UINT64_C(0x1);
  r.ctrl.handshake().onPeerHello(skewed);
  TEST_ASSERT_FALSE(r.ctrl.authorized());
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_full_boot_to_run_and_timeout);
  RUN_TEST(test_schema_mismatch_blocks_run);
  return UNITY_END();
}
