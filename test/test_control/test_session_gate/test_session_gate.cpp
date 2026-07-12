// native_control suite — the controller's run-authorization gate (design.md §9).
//
// Feeds decoded Heartbeats straight into SessionGate (the MessageRouter path is
// covered elsewhere) and advances a FakeClock to exercise the freshness window,
// session filter, enable/no-latch behavior, and the four-term authorization.
#include <unity.h>

#include "frame_link.h"
#include "handshake.h"
#include "helpers/fake_clock.h"
#include "helpers/pipe_transport.h"
#include "link_params.h"
#include "message_router.h"
#include "messages.h"
#include "oven.pb.h"
#include "schema.h"
#include "session_gate.h"

// A matched Handshake needs a FrameLink; the send side is unused here. IMessage-
// Observer is concrete (all no-op defaults), so it serves as an inert sink.
struct GateFixture {
  LoopbackPipe pipe;
  protocol::IMessageObserver sink;
  protocol::MessageRouter router;
  FakeClock clk;
  protocol::FrameLink link;
  protocol::Handshake hs;
  SessionGate gate;

  GateFixture() : router(sink), link(pipe.a(), TF_MASTER, router), hs(link, clk), gate(clk, hs) {}

  void makeMatched() { hs.onPeerHello(hello(protocol::kProtoVer, protocol::kSchemaHash)); }

  static oven_Hello hello(uint32_t proto_ver, uint64_t schema_hash) {
    oven_Hello h = oven_Hello_init_default;
    h.proto_ver = proto_ver;
    h.schema_hash = schema_hash;
    return h;
  }
};

static oven_Heartbeat hb(uint32_t session, uint32_t seq, bool enable) {
  oven_Heartbeat h = oven_Heartbeat_init_default;
  h.session = session;
  h.seq = seq;
  h.enable = enable;
  return h;
}

void setUp(void) {}
void tearDown(void) {}

// Adopting a session then receiving its heartbeat authorizes; before any HB
// arrives the gate stays closed (monitor unfed).
void test_start_adopts_and_hb_authorizes(void) {
  GateFixture f;
  f.makeMatched();
  f.gate.adoptSession(42);
  TEST_ASSERT_FALSE(f.gate.authorized()); // no heartbeat yet
  f.gate.onHeartbeat(hb(42, 0, true));
  TEST_ASSERT_TRUE(f.gate.authorized());
}

// A heartbeat just inside the command-timeout keeps authorization.
void test_fresh_hb_within_timeout(void) {
  GateFixture f;
  f.makeMatched();
  f.gate.adoptSession(1);
  f.gate.onHeartbeat(hb(1, 0, true));
  f.clk.advance(protocol::kCommandTimeoutMs - 1);
  TEST_ASSERT_TRUE(f.gate.authorized());
}

// Silence past the command-timeout de-authorizes (link lost).
void test_silence_past_timeout_deauthorizes(void) {
  GateFixture f;
  f.makeMatched();
  f.gate.adoptSession(1);
  f.gate.onHeartbeat(hb(1, 0, true));
  f.clk.advance(protocol::kCommandTimeoutMs);
  TEST_ASSERT_FALSE(f.gate.authorized());
}

// A heartbeat for a different session is ignored — authorizes nothing (the
// rebooted/stale-CYD case).
void test_unknown_session_hb_ignored(void) {
  GateFixture f;
  f.makeMatched();
  f.gate.adoptSession(42);
  f.gate.onHeartbeat(hb(99, 0, true)); // wrong session
  TEST_ASSERT_FALSE(f.gate.authorized());
}

// A heartbeat before any session is adopted authorizes nothing.
void test_hb_without_active_session_ignored(void) {
  GateFixture f;
  f.makeMatched();
  f.gate.onHeartbeat(hb(7, 0, true));
  TEST_ASSERT_FALSE(f.gate.authorized());
}

// enable=false de-authorizes even while the heartbeat is fresh.
void test_enable_false_deauthorizes(void) {
  GateFixture f;
  f.makeMatched();
  f.gate.adoptSession(1);
  f.gate.onHeartbeat(hb(1, 0, false));
  TEST_ASSERT_FALSE(f.gate.authorized());
}

// No latch: a fresh enable=false immediately overrides a prior enable=true.
void test_no_latch_enable_true_then_false(void) {
  GateFixture f;
  f.makeMatched();
  f.gate.adoptSession(1);
  f.gate.onHeartbeat(hb(1, 0, true));
  TEST_ASSERT_TRUE(f.gate.authorized());
  f.gate.onHeartbeat(hb(1, 1, false));
  TEST_ASSERT_FALSE(f.gate.authorized());
}

// All four terms are required: from an authorized state, flipping any single one
// (enable, freshness, handshake match, active session) closes the gate.
void test_authorization_requires_all_terms(void) {
  GateFixture f;
  f.makeMatched();
  f.gate.adoptSession(7);
  f.gate.onHeartbeat(hb(7, 0, true));
  TEST_ASSERT_TRUE(f.gate.authorized());

  // enable
  f.gate.onHeartbeat(hb(7, 1, false));
  TEST_ASSERT_FALSE(f.gate.authorized());
  f.gate.onHeartbeat(hb(7, 2, true));
  TEST_ASSERT_TRUE(f.gate.authorized());

  // freshness
  f.clk.advance(protocol::kCommandTimeoutMs);
  TEST_ASSERT_FALSE(f.gate.authorized());
  f.gate.onHeartbeat(hb(7, 3, true));
  TEST_ASSERT_TRUE(f.gate.authorized());

  // handshake match (peer reboots built against a different .proto)
  f.hs.onPeerHello(GateFixture::hello(protocol::kProtoVer, protocol::kSchemaHash ^ UINT64_C(0x1)));
  TEST_ASSERT_FALSE(f.gate.authorized());
  f.hs.onPeerHello(GateFixture::hello(protocol::kProtoVer, protocol::kSchemaHash));
  TEST_ASSERT_TRUE(f.gate.authorized());

  // active session
  f.gate.clearSession();
  TEST_ASSERT_FALSE(f.gate.authorized());
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_start_adopts_and_hb_authorizes);
  RUN_TEST(test_fresh_hb_within_timeout);
  RUN_TEST(test_silence_past_timeout_deauthorizes);
  RUN_TEST(test_unknown_session_hb_ignored);
  RUN_TEST(test_hb_without_active_session_ignored);
  RUN_TEST(test_enable_false_deauthorizes);
  RUN_TEST(test_no_latch_enable_true_then_false);
  RUN_TEST(test_authorization_requires_all_terms);
  return UNITY_END();
}
