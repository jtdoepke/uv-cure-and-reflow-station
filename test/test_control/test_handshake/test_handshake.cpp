// native_control suite — the schema-hash handshake gate (design.md §9).
//
// Pure-gate cases drive Handshake::onPeerHello() directly with crafted Hellos
// (the only way to inject a *mismatching* schema hash, since a real Handshake
// always sends our own). The over-the-wire case runs two Handshakes across a
// LoopbackPipe through MessageRouter to prove the boot exchange, and a one-sided
// case proves the retransmit-until-peer-seen behavior.
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

// Forwards decoded Hellos into a Handshake (what the facades do in production).
struct HandshakeObserver : protocol::IMessageObserver {
  protocol::Handshake &hs;
  int hellos = 0;
  explicit HandshakeObserver(protocol::Handshake &h) : hs(h) {}
  void onHello(const oven_Hello &h) override {
    ++hellos;
    hs.onPeerHello(h);
  }
};

// Counts Hellos without answering — used to watch one side retransmit.
struct CountingObserver : protocol::IMessageObserver {
  int hellos = 0;
  void onHello(const oven_Hello &) override { ++hellos; }
};

// One endpoint's receive stack: router -> observer -> handshake, plus the link.
// Built in dependency order using MessageRouter's late-bound observer.
struct HandshakeNode {
  FakeClock clk;
  protocol::MessageRouter router;
  protocol::FrameLink link;
  protocol::Handshake hs;
  HandshakeObserver obs;

  HandshakeNode(ISerialTransport &transport, TF_Peer peer)
      : router(), link(transport, peer, router), hs(link, clk), obs(hs) {
    router.setObserver(obs);
  }
};

void setUp(void) {}
void tearDown(void) {}

static oven_Hello makeHello(uint32_t proto_ver, uint64_t schema_hash, uint32_t boot_nonce = 0) {
  oven_Hello h = oven_Hello_init_default;
  h.proto_ver = proto_ver;
  h.schema_hash = schema_hash;
  h.boot_nonce = boot_nonce;
  return h;
}

// A well-formed Hello from a peer on our exact schema, distinguished only by which
// boot it came from.
static oven_Hello peerHelloFromBoot(uint32_t boot_nonce) {
  return makeHello(protocol::kProtoVer, protocol::kSchemaHash, boot_nonce);
}

// A single Handshake wired to a link, for the pure-gate cases.
struct SoloHandshake {
  LoopbackPipe pipe;
  CountingObserver obs;
  protocol::MessageRouter router;
  FakeClock clk;
  protocol::FrameLink link;
  protocol::Handshake hs;
  SoloHandshake() : router(obs), link(pipe.a(), TF_MASTER, router), hs(link, clk) {}
};

// A matching peer Hello (same proto_ver + schema_hash) flips matched() true.
void test_matching_hello_matches(void) {
  SoloHandshake s;
  s.hs.onPeerHello(makeHello(protocol::kProtoVer, protocol::kSchemaHash));
  TEST_ASSERT_TRUE(s.hs.sawPeer());
  TEST_ASSERT_TRUE(s.hs.matched());
}

// A different schema_hash fails closed even with a matching proto_ver.
void test_schema_hash_mismatch_fails_closed(void) {
  SoloHandshake s;
  s.hs.onPeerHello(makeHello(protocol::kProtoVer, protocol::kSchemaHash ^ UINT64_C(0x1)));
  TEST_ASSERT_TRUE(s.hs.sawPeer());  // we heard the peer...
  TEST_ASSERT_FALSE(s.hs.matched()); // ...but refuse to trust it
}

// A proto_ver bump also fails closed (semantic skew the hash can't express).
void test_proto_ver_mismatch_fails_closed(void) {
  SoloHandshake s;
  s.hs.onPeerHello(makeHello(protocol::kProtoVer + 1, protocol::kSchemaHash));
  TEST_ASSERT_FALSE(s.hs.matched());
}

// Fail-closed default: never having heard a peer reads as unmatched.
void test_never_received_hello_is_unmatched(void) {
  SoloHandshake s;
  TEST_ASSERT_FALSE(s.hs.sawPeer());
  TEST_ASSERT_FALSE(s.hs.matched());
}

// A later mismatching Hello (rebooted peer on a different .proto) drops match.
void test_mismatch_after_match_rearms_closed(void) {
  SoloHandshake s;
  s.hs.onPeerHello(makeHello(protocol::kProtoVer, protocol::kSchemaHash));
  TEST_ASSERT_TRUE(s.hs.matched());
  s.hs.onPeerHello(makeHello(protocol::kProtoVer, protocol::kSchemaHash ^ UINT64_C(0xABCD)));
  TEST_ASSERT_FALSE(s.hs.matched());
}

// Both boards exchange Hello over the pipe and end up matched.
void test_handshake_matches_over_pipe(void) {
  LoopbackPipe pipe;
  HandshakeNode cyd(pipe.a(), TF_MASTER);
  HandshakeNode ctrl(pipe.b(), TF_SLAVE);

  cyd.hs.begin(0xC1D00001);
  ctrl.hs.begin(0xC7100001);
  cyd.link.poll();
  ctrl.link.poll();

  TEST_ASSERT_TRUE(cyd.hs.sawPeer());
  TEST_ASSERT_TRUE(ctrl.hs.sawPeer());
  TEST_ASSERT_TRUE(cyd.hs.matched());
  TEST_ASSERT_TRUE(ctrl.hs.matched());
}

// --- §9 re-sync: hearing a peer is not the same as being heard ----------------
// service() goes quiet the moment saw_peer_ flips, so whoever hears first must answer
// or the other side announces into a void forever. Found on the A8 bench twice over:
// the CYD (~3 s slower to boot) sat at sawPeer=0 while the controller read matched=1,
// and resetting the controller alone left it matched=0.

// First contact is answered: the peer has no idea we exist, and our own announce loop
// is about to stop precisely because we just heard it.
void test_first_contact_is_answered(void) {
  SoloHandshake s;
  s.hs.begin(0xA5A5A5A5);

  // Nothing reads this pipe, so pending bytes only ever grow: a Hello we emit shows up
  // as an increase. Baseline after our own boot Hello.
  const size_t before = s.pipe.pendingAToB();
  s.hs.onPeerHello(peerHelloFromBoot(0x1111));
  TEST_ASSERT_TRUE(s.pipe.pendingAToB() > before);
}

// A Hello from a *new* boot of an already-known peer is answered immediately, without
// waiting on the rate limit — a rebooted peer certainly does not know us.
void test_peer_reboot_is_answered_immediately(void) {
  SoloHandshake s;
  s.hs.begin(0xA5A5A5A5);
  s.hs.onPeerHello(peerHelloFromBoot(0x1111));

  const size_t before = s.pipe.pendingAToB();
  s.hs.onPeerHello(peerHelloFromBoot(0x2222)); // same peer, new boot
  TEST_ASSERT_TRUE(s.pipe.pendingAToB() > before);
}

// A repeated Hello inside the rate-limit window is NOT answered. This is what stops two
// live boards from answering each other's answers forever at wire speed.
void test_repeated_hello_is_rate_limited(void) {
  SoloHandshake s;
  s.hs.begin(0xA5A5A5A5);
  s.hs.onPeerHello(peerHelloFromBoot(0x1111)); // answered (first contact)

  const size_t before = s.pipe.pendingAToB();
  s.hs.onPeerHello(peerHelloFromBoot(0x1111));
  s.hs.onPeerHello(peerHelloFromBoot(0x1111));
  TEST_ASSERT_EQUAL_size_t(before, s.pipe.pendingAToB());
}

// ...but once the window elapses a repeat IS answered again, so a reply that got lost on
// the wire recovers: the peer keeps announcing and we keep answering until one lands.
void test_repeated_hello_answered_again_after_window(void) {
  SoloHandshake s;
  s.hs.begin(0xA5A5A5A5);
  s.hs.onPeerHello(peerHelloFromBoot(0x1111));

  const size_t before = s.pipe.pendingAToB();
  s.clk.advance(protocol::kHelloRetryMs);
  s.hs.onPeerHello(peerHelloFromBoot(0x1111));
  TEST_ASSERT_TRUE(s.pipe.pendingAToB() > before);
}

// End to end, and the case the bench actually hit: two boards match, then ONE reboots
// (a fresh Handshake with a new nonce, the other left running). Without the answer the
// rebooted side Hellos into silence and never matches again — which on hardware meant
// every watchdog reset took the link down permanently.
void test_one_sided_reboot_rematches_over_pipe(void) {
  LoopbackPipe pipe;
  HandshakeNode cyd(pipe.a(), TF_MASTER);

  {
    HandshakeNode ctrl(pipe.b(), TF_SLAVE);
    cyd.hs.begin(0xC1D00001);
    ctrl.hs.begin(0xC7100001);
    cyd.link.poll();
    ctrl.link.poll();
    TEST_ASSERT_TRUE(cyd.hs.matched());
    TEST_ASSERT_TRUE(ctrl.hs.matched());
  }
  // The CYD has now stopped announcing: it has seen a peer.
  cyd.hs.service();
  cyd.hs.service();

  // Controller reboots: fresh state, fresh nonce, same wire.
  HandshakeNode rebooted(pipe.b(), TF_SLAVE);
  rebooted.hs.begin(0xC7100002);
  cyd.link.poll();      // CYD sees the new nonce and answers
  rebooted.link.poll(); // rebooted controller receives that answer

  TEST_ASSERT_TRUE(rebooted.hs.matched());
  TEST_ASSERT_TRUE(cyd.hs.matched());
}

// One side keeps re-announcing Hello every kHelloRetryMs until the peer answers.
void test_service_retransmits_until_peer_seen(void) {
  LoopbackPipe pipe;
  FakeClock clk;
  CountingObserver peer_obs; // the peer only counts; it never answers
  protocol::MessageRouter peer_router(peer_obs);
  protocol::FrameLink peer_link(pipe.b(), TF_SLAVE, peer_router);

  protocol::MessageRouter my_router;
  protocol::FrameLink my_link(pipe.a(), TF_MASTER, my_router);
  protocol::Handshake hs(my_link, clk);
  HandshakeObserver my_obs(hs);
  my_router.setObserver(my_obs);

  hs.service(); // first send
  peer_link.poll();
  TEST_ASSERT_EQUAL_INT(1, peer_obs.hellos);

  clk.advance(protocol::kHelloRetryMs - 1); // too soon
  hs.service();
  peer_link.poll();
  TEST_ASSERT_EQUAL_INT(1, peer_obs.hellos);

  clk.advance(1); // now at the retry interval
  hs.service();
  peer_link.poll();
  TEST_ASSERT_EQUAL_INT(2, peer_obs.hellos);

  // The peer finally announces itself. Two distinct things follow, and the counts keep
  // them apart: we answer it exactly once (it cannot know us yet — this observer never
  // replied, so from our side this is first contact)...
  hs.onPeerHello(makeHello(protocol::kProtoVer, protocol::kSchemaHash));
  peer_link.poll();
  TEST_ASSERT_EQUAL_INT(3, peer_obs.hellos);

  // ...and from then on service() announces no more, however long we wait.
  clk.advance(protocol::kHelloRetryMs * 3);
  hs.service();
  peer_link.poll();
  TEST_ASSERT_EQUAL_INT(3, peer_obs.hellos);
  TEST_ASSERT_TRUE(hs.matched());
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_matching_hello_matches);
  RUN_TEST(test_schema_hash_mismatch_fails_closed);
  RUN_TEST(test_proto_ver_mismatch_fails_closed);
  RUN_TEST(test_never_received_hello_is_unmatched);
  RUN_TEST(test_mismatch_after_match_rearms_closed);
  RUN_TEST(test_handshake_matches_over_pipe);
  RUN_TEST(test_first_contact_is_answered);
  RUN_TEST(test_peer_reboot_is_answered_immediately);
  RUN_TEST(test_repeated_hello_is_rate_limited);
  RUN_TEST(test_repeated_hello_answered_again_after_window);
  RUN_TEST(test_one_sided_reboot_rematches_over_pipe);
  RUN_TEST(test_service_retransmits_until_peer_seen);
  return UNITY_END();
}
