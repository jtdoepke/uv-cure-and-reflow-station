// native_control suite — the seq/ACK-NAK setup path (design.md §9). Runs a real
// ReliableSender (CYD) against a real SetupResponder (controller) over a
// LoopbackPipe, with a FlakyTransport on the CYD's TX to drop frames for the
// retry cases.
#include <unity.h>

#include "frame_link.h"
#include "helpers/fake_clock.h"
#include "helpers/flaky_transport.h"
#include "helpers/pipe_transport.h"
#include "link_params.h"
#include "message_router.h"
#include "messages.h"
#include "oven.pb.h"
#include "reliable_sender.h"
#include "setup_responder.h"

using protocol::ReliableSender;

// Forwards decoded acks/naks into the sender; also counts them raw so the
// duplicate-suppression test can see re-acks the sender itself would ignore.
struct CydObserver : protocol::IMessageObserver {
  ReliableSender &s;
  int acks = 0;
  int naks = 0;
  explicit CydObserver(ReliableSender &s_) : s(s_) {}
  void onAck(const oven_Ack &a) override {
    ++acks;
    s.onAck(a);
  }
  void onNak(const oven_Nak &n) override {
    ++naks;
    s.onNak(n);
  }
};

struct CtrlObserver : protocol::IMessageObserver {
  protocol::SetupResponder &r;
  explicit CtrlObserver(protocol::SetupResponder &r_) : r(r_) {}
  void onRecipe(const oven_Recipe &m) override { r.onRecipe(m); }
  void onStart(const oven_Start &m) override { r.onStart(m); }
};

struct AcceptCounter : protocol::ISetupSink {
  int recipes = 0;
  int starts = 0;
  uint32_t last_start_seq = 0;
  uint32_t last_start_session = 0;
  void onRecipeAccepted(const oven_Recipe &) override { ++recipes; }
  void onStartAccepted(const oven_Start &s) override {
    ++starts;
    last_start_seq = s.seq;
    last_start_session = s.session;
  }
};

// Rejects everything with a fixed reason (stands in for A7's real checks).
struct RejectValidator : protocol::ISetupValidator {
  oven_NakReason reason;
  explicit RejectValidator(oven_NakReason r) : reason(r) {}
  bool validateRecipe(const oven_Recipe &, oven_NakReason &out) override {
    out = reason;
    return false;
  }
  bool validateStart(const oven_Start &, oven_NakReason &out) override {
    out = reason;
    return false;
  }
};

struct Rig {
  LoopbackPipe pipe;
  FlakyTransport cyd_tx;
  FakeClock clk;
  AcceptCounter sink;
  protocol::MessageRouter ctrl_router;
  protocol::FrameLink ctrl_link;
  protocol::SetupResponder responder;
  CtrlObserver ctrl_obs;
  protocol::MessageRouter cyd_router;
  protocol::FrameLink cyd_link;
  protocol::ReliableSender sender;
  CydObserver cyd_obs;

  explicit Rig(protocol::ISetupValidator &v)
      : cyd_tx(pipe.a()), ctrl_router(), ctrl_link(pipe.b(), TF_SLAVE, ctrl_router),
        responder(ctrl_link, v), ctrl_obs(responder), cyd_router(),
        cyd_link(cyd_tx, TF_MASTER, cyd_router), sender(cyd_link, clk), cyd_obs(sender) {
    responder.setSink(sink);
    ctrl_router.setObserver(ctrl_obs);
    cyd_router.setObserver(cyd_obs);
  }

  void pumpCtrl() { ctrl_link.poll(); }
  void pumpCyd() { cyd_link.poll(); }
  void exchange() {
    ctrl_link.poll();
    cyd_link.poll();
  }
};

static oven_Recipe simpleRecipe(uint32_t id) {
  oven_Recipe rec = oven_Recipe_init_default;
  rec.id = id;
  rec.mode = oven_Mode_MODE_CURE;
  rec.segments_count = 1;
  rec.segments[0].dur_ms = 1000;
  rec.segments[0].heat_c = 80.0F;
  rec.segments[0].interp = oven_Interp_INTERP_HOLD;
  return rec;
}

static int stateInt(ReliableSender::State s) {
  return static_cast<int>(s);
}

void setUp(void) {}
void tearDown(void) {}

// A validated Recipe is Acked exactly once.
void test_ack_happy_path(void) {
  protocol::AcceptAllValidator v;
  Rig r(v);

  TEST_ASSERT_TRUE(r.sender.sendRecipe(simpleRecipe(1)));
  r.pumpCtrl(); // controller receives + acks
  r.pumpCyd();  // CYD receives ack

  TEST_ASSERT_EQUAL_INT(stateInt(ReliableSender::State::Acked), stateInt(r.sender.state()));
  TEST_ASSERT_EQUAL_INT(1, r.sink.recipes);
  TEST_ASSERT_EQUAL_INT(1, r.cyd_obs.acks);
}

// A rejected Recipe surfaces Nakd with the reason and applies no side effect.
void test_nak_path(void) {
  RejectValidator v(oven_NakReason_NAK_OUT_OF_RANGE);
  Rig r(v);

  TEST_ASSERT_TRUE(r.sender.sendRecipe(simpleRecipe(1)));
  r.pumpCtrl();
  r.pumpCyd();

  TEST_ASSERT_EQUAL_INT(stateInt(ReliableSender::State::Nakd), stateInt(r.sender.state()));
  TEST_ASSERT_EQUAL_INT(oven_NakReason_NAK_OUT_OF_RANGE, r.sender.lastNakReason());
  TEST_ASSERT_EQUAL_INT(0, r.sink.recipes);
}

// Dropped sends are retried until one lands and is Acked.
void test_retry_until_ack(void) {
  protocol::AcceptAllValidator v;
  Rig r(v);

  r.cyd_tx.drop_tx = true; // swallow the initial send and the first retry
  TEST_ASSERT_TRUE(r.sender.sendRecipe(simpleRecipe(1)));
  r.pumpCtrl();
  TEST_ASSERT_EQUAL_INT(0, r.sink.recipes);

  r.clk.advance(protocol::kSetupAckTimeoutMs);
  r.sender.service(); // retry #1, still dropped
  r.pumpCtrl();
  TEST_ASSERT_EQUAL_INT(0, r.sink.recipes);
  TEST_ASSERT_EQUAL_INT(1, r.sender.attempts());

  r.cyd_tx.drop_tx = false;
  r.clk.advance(protocol::kSetupAckTimeoutMs);
  r.sender.service(); // retry #2 gets through
  r.exchange();

  TEST_ASSERT_EQUAL_INT(stateInt(ReliableSender::State::Acked), stateInt(r.sender.state()));
  TEST_ASSERT_EQUAL_INT(1, r.sink.recipes);
  TEST_ASSERT_EQUAL_INT(2, r.sender.attempts());
}

// After the initial send plus kSetupMaxRetries resends with no ack, the sender
// gives up (Failed).
void test_give_up_after_max_retries(void) {
  protocol::AcceptAllValidator v;
  Rig r(v);

  r.cyd_tx.drop_tx = true; // everything is lost
  TEST_ASSERT_TRUE(r.sender.sendRecipe(simpleRecipe(1)));

  for (int i = 0; i < protocol::kSetupMaxRetries; ++i) {
    r.clk.advance(protocol::kSetupAckTimeoutMs);
    r.sender.service();
    TEST_ASSERT_EQUAL_INT(stateInt(ReliableSender::State::Pending), stateInt(r.sender.state()));
  }
  r.clk.advance(protocol::kSetupAckTimeoutMs);
  r.sender.service(); // retry budget exhausted

  TEST_ASSERT_EQUAL_INT(stateInt(ReliableSender::State::Failed), stateInt(r.sender.state()));
  TEST_ASSERT_EQUAL_INT(protocol::kSetupMaxRetries, r.sender.attempts());
  TEST_ASSERT_EQUAL_INT(0, r.sink.recipes);
}

// A duplicate Recipe (same seq — what a dropped Ack causes) is acked again but
// applied only once.
void test_duplicate_seq_suppressed(void) {
  protocol::AcceptAllValidator v;
  Rig r(v);

  oven_Recipe rec = simpleRecipe(1);
  rec.seq = 5;
  r.responder.onRecipe(rec);
  r.responder.onRecipe(rec); // retransmit of the same command
  r.pumpCyd();               // deliver both acks

  TEST_ASSERT_EQUAL_INT(1, r.sink.recipes); // side effect once
  TEST_ASSERT_EQUAL_INT(2, r.cyd_obs.acks); // re-acked both
}

// The shared seq counter advances Recipe -> Start; the controller sees Start's
// seq as the next value.
void test_start_seq_follows_recipe(void) {
  protocol::AcceptAllValidator v;
  Rig r(v);

  r.sender.sendRecipe(simpleRecipe(1)); // seq 1
  r.exchange();
  TEST_ASSERT_EQUAL_INT(stateInt(ReliableSender::State::Acked), stateInt(r.sender.state()));
  TEST_ASSERT_EQUAL_UINT32(1, r.sender.pendingSeq());

  oven_Start st = oven_Start_init_default;
  st.session = 7;
  st.recipe_id = 1;
  TEST_ASSERT_TRUE(r.sender.sendStart(st)); // seq 2
  r.exchange();

  TEST_ASSERT_EQUAL_INT(stateInt(ReliableSender::State::Acked), stateInt(r.sender.state()));
  TEST_ASSERT_EQUAL_UINT32(2, r.sender.pendingSeq());
  TEST_ASSERT_EQUAL_INT(1, r.sink.starts);
  TEST_ASSERT_EQUAL_UINT32(2, r.sink.last_start_seq);
  TEST_ASSERT_EQUAL_UINT32(7, r.sink.last_start_session);
}

// Swaps a fresh sender onto the same wire, standing in for a CYD reboot: the
// controller keeps running (and keeps its cached seq), the CYD's counter restarts.
struct RebootedCyd {
  protocol::ReliableSender sender;
  CydObserver obs;
  RebootedCyd(Rig &r) : sender(r.cyd_link, r.clk), obs(sender) { r.cyd_router.setObserver(obs); }
};

static oven_Start startFor(uint32_t session) {
  oven_Start st = oven_Start_init_default;
  st.session = session;
  st.recipe_id = 1;
  return st;
}

// WHY setSeqBase exists — the failure it prevents. seq is monotonic only within a
// boot, but the responder's dedup treats it as globally unique, so an unseeded
// reboot re-sends seq 1 and the controller mistakes a brand-new Start for a
// replay. Note what the CYD sees: Acked. The link looks healthy while the session
// was silently never adopted, so the oven would never authorize (or, worse, would
// stay authorized on the pre-reboot session). Delete setSeqBase and this passes
// with `starts == 1`.
void test_reboot_without_seq_base_is_deduped(void) {
  protocol::AcceptAllValidator v;
  Rig r(v);

  TEST_ASSERT_TRUE(r.sender.sendStart(startFor(0xAAA))); // seq 1
  r.exchange();
  TEST_ASSERT_EQUAL_INT(1, r.sink.starts);
  TEST_ASSERT_EQUAL_UINT32(0xAAA, r.sink.last_start_session);

  RebootedCyd fresh(r);                                      // seq_ back to 0
  TEST_ASSERT_TRUE(fresh.sender.sendStart(startFor(0xBBB))); // seq 1 again -> collision
  r.exchange();

  // The sender is told it landed...
  TEST_ASSERT_EQUAL_INT(stateInt(ReliableSender::State::Acked), stateInt(fresh.sender.state()));
  // ...but the controller replayed the cached verdict and skipped the side effect:
  // no new Start, and the session is still the pre-reboot one.
  TEST_ASSERT_EQUAL_INT(1, r.sink.starts);
  TEST_ASSERT_EQUAL_UINT32(0xAAA, r.sink.last_start_session);
}

// Seeding the counter per boot dodges the single-slot dedup: the rebooted CYD's
// Start is accepted and its session actually adopted.
void test_reboot_with_seq_base_readopts_session(void) {
  protocol::AcceptAllValidator v;
  Rig r(v);

  r.sender.setSeqBase(1000);
  TEST_ASSERT_TRUE(r.sender.sendStart(startFor(0xAAA))); // seq 1001
  r.exchange();
  TEST_ASSERT_EQUAL_INT(1, r.sink.starts);
  TEST_ASSERT_EQUAL_UINT32(1001, r.sink.last_start_seq);

  RebootedCyd fresh(r);
  fresh.sender.setSeqBase(5000);
  TEST_ASSERT_TRUE(fresh.sender.sendStart(startFor(0xBBB))); // seq 5001 -> no collision
  r.exchange();

  TEST_ASSERT_EQUAL_INT(stateInt(ReliableSender::State::Acked), stateInt(fresh.sender.state()));
  TEST_ASSERT_EQUAL_INT(2, r.sink.starts); // accepted, not deduped
  TEST_ASSERT_EQUAL_UINT32(5001, r.sink.last_start_seq);
  TEST_ASSERT_EQUAL_UINT32(0xBBB, r.sink.last_start_session); // the new session landed
}

// The seed is a base, not the first seq: the next command is base + 1, and the
// Recipe -> Start progression carries on from there.
void test_seq_base_offsets_subsequent_commands(void) {
  protocol::AcceptAllValidator v;
  Rig r(v);

  r.sender.setSeqBase(42);
  r.sender.sendRecipe(simpleRecipe(1));
  r.exchange();
  TEST_ASSERT_EQUAL_UINT32(43, r.sender.pendingSeq());

  r.sender.sendStart(startFor(7));
  r.exchange();
  TEST_ASSERT_EQUAL_UINT32(44, r.sender.pendingSeq());
  TEST_ASSERT_EQUAL_UINT32(44, r.sink.last_start_seq);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_ack_happy_path);
  RUN_TEST(test_nak_path);
  RUN_TEST(test_retry_until_ack);
  RUN_TEST(test_give_up_after_max_retries);
  RUN_TEST(test_duplicate_seq_suppressed);
  RUN_TEST(test_start_seq_follows_recipe);
  RUN_TEST(test_reboot_without_seq_base_is_deduped);
  RUN_TEST(test_reboot_with_seq_base_readopts_session);
  RUN_TEST(test_seq_base_offsets_subsequent_commands);
  return UNITY_END();
}
