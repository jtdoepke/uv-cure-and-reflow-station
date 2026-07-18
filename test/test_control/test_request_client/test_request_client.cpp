// native_control suite — the request/reply management path (design.md §9, added 2026-07-17
// with the §2 "CYD is a UI remote" split). Runs a real RequestClient (CYD) against a minimal
// fake profile/settings responder (controller) over a LoopbackPipe, with FlakyTransports on
// both sides for the retry + dedup cases. The REAL ProfileResponder/SettingsResponder land with
// the controller stores (Wave R2); this suite pins the transport + seq-correlation + dedup
// contract those responders and the CYD's ProfileClient rely on.
#include <unity.h>

#include <cstring>

#include "codec.h"
#include "frame_link.h"
#include "helpers/fake_clock.h"
#include "helpers/flaky_transport.h"
#include "helpers/pipe_transport.h"
#include "link_params.h"
#include "message_router.h"
#include "messages.h"
#include "oven.pb.h"
#include "request_client.h"

using protocol::RequestClient;

// Minimal stand-in for R2's ProfileResponder: replies to each management request with a canned
// payload echoing seq, and dedups a retransmitted request by caching the last {seq -> reply} and
// replaying it WITHOUT re-running the side effect (the invariant the real responder must hold).
struct FakeResponder : protocol::IMessageObserver {
  protocol::FrameLink &link;
  int puts = 0; // side-effect counter — proves a deduped resend does not double-apply
  int lists = 0;
  int gets = 0;

  bool have_last = false;
  uint32_t last_seq = 0;
  uint8_t last_type = 0;
  uint8_t last_buf[oven_ProfileList_size] = {0}; // ProfileList is the largest reply
  size_t last_len = 0;

  explicit FakeResponder(protocol::FrameLink &l) : link(l) {}

  // Replay the cached reply for a duplicate seq; returns true if it was a duplicate.
  bool dedup(uint32_t seq) {
    if (have_last && seq == last_seq) {
      link.send(last_type, last_buf, last_len);
      return true;
    }
    return false;
  }

  void reply(uint32_t seq, uint8_t type, const pb_msgdesc_t *fields, const void *msg) {
    uint8_t buf[oven_ProfileList_size];
    size_t len = 0;
    if (!protocol::encode(fields, msg, buf, sizeof(buf), len)) {
      return;
    }
    last_seq = seq;
    last_type = type;
    last_len = len;
    have_last = true;
    std::memcpy(last_buf, buf, len);
    link.send(type, buf, len);
  }

  void onProfileListReq(const oven_ProfileListReq &m) override {
    if (dedup(m.seq)) {
      return;
    }
    ++lists;
    oven_ProfileList r = oven_ProfileList_init_zero;
    r.seq = m.seq;
    r.profiles_count = 2;
    std::strncpy(r.profiles[0].name, "LF-245", sizeof(r.profiles[0].name) - 1);
    r.profiles[0].stock = true;
    r.profiles[0].peak_c = 245.0F;
    r.profiles[0].total_s = 370;
    std::strncpy(r.profiles[1].name, "MyBoard", sizeof(r.profiles[1].name) - 1);
    r.profiles[1].peak_c = 240.0F;
    r.profiles[1].total_s = 360;
    reply(m.seq, protocol::kTfTypeProfileList, oven_ProfileList_fields, &r);
  }

  void onProfileGetReq(const oven_ProfileGetReq &m) override {
    if (dedup(m.seq)) {
      return;
    }
    ++gets;
    oven_ProfileData r = oven_ProfileData_init_zero;
    r.seq = m.seq;
    r.has_profile = true;
    r.profile.mode = m.mode;
    std::strncpy(r.profile.name, m.name, sizeof(r.profile.name) - 1);
    r.profile.phases_count = 1;
    r.profile.phases[0].target_c = 245.0F;
    r.profile.phases[0].fan_mode = oven_FanMode_FAN_MODE_AUTO;
    reply(m.seq, protocol::kTfTypeProfileData, oven_ProfileData_fields, &r);
  }

  void onProfilePut(const oven_ProfilePut &m) override {
    if (dedup(m.seq)) {
      return;
    }
    ++puts;
    oven_MgmtResult r = oven_MgmtResult_init_zero;
    r.seq = m.seq;
    r.ok = true;
    reply(m.seq, protocol::kTfTypeMgmtResult, oven_MgmtResult_fields, &r);
  }

  void onSettingsGetReq(const oven_SettingsGetReq &m) override {
    if (dedup(m.seq)) {
      return;
    }
    oven_SettingsData r = oven_SettingsData_init_zero;
    r.seq = m.seq;
    r.has_settings = true;
    r.settings.reflow_max_cap = 250;
    reply(m.seq, protocol::kTfTypeSettingsData, oven_SettingsData_fields, &r);
  }
};

// CYD-side reply sink: forwards every management reply's seq into the client (its only job) and
// captures the payloads the tests assert on.
struct CydObserver : protocol::IMessageObserver {
  RequestClient &c;
  int lists = 0, datas = 0, results = 0, settings = 0;
  oven_ProfileList last_list = oven_ProfileList_init_zero;
  oven_ProfileData last_data = oven_ProfileData_init_zero;
  oven_MgmtResult last_result = oven_MgmtResult_init_zero;
  oven_SettingsData last_settings = oven_SettingsData_init_zero;

  explicit CydObserver(RequestClient &c_) : c(c_) {}
  void onProfileList(const oven_ProfileList &m) override {
    ++lists;
    last_list = m;
    c.onReply(m.seq);
  }
  void onProfileData(const oven_ProfileData &m) override {
    ++datas;
    last_data = m;
    c.onReply(m.seq);
  }
  void onMgmtResult(const oven_MgmtResult &m) override {
    ++results;
    last_result = m;
    c.onReply(m.seq);
  }
  void onSettingsData(const oven_SettingsData &m) override {
    ++settings;
    last_settings = m;
    c.onReply(m.seq);
  }
};

struct Rig {
  LoopbackPipe pipe;
  FlakyTransport cyd_tx;  // CYD -> controller (drop a request)
  FlakyTransport ctrl_tx; // controller -> CYD (drop a reply)
  FakeClock clk;
  protocol::MessageRouter ctrl_router;
  protocol::FrameLink ctrl_link;
  FakeResponder responder;
  protocol::MessageRouter cyd_router;
  protocol::FrameLink cyd_link;
  RequestClient client;
  CydObserver cyd_obs;

  Rig()
      : cyd_tx(pipe.a()), ctrl_tx(pipe.b()), ctrl_router(),
        ctrl_link(ctrl_tx, TF_SLAVE, ctrl_router), responder(ctrl_link), cyd_router(),
        cyd_link(cyd_tx, TF_MASTER, cyd_router), client(cyd_link, clk), cyd_obs(client) {
    ctrl_router.setObserver(responder);
    cyd_router.setObserver(cyd_obs);
  }

  void pumpCtrl() { ctrl_link.poll(); }
  void pumpCyd() { cyd_link.poll(); }
  void exchange() {
    ctrl_link.poll();
    cyd_link.poll();
  }
};

// Encode `msg` (stamping a fresh seq) and hand it to the client under `type`. Returns the seq.
template <typename Msg>
static uint32_t sendReq(RequestClient &c, const pb_msgdesc_t *fields, uint8_t type, Msg &msg) {
  uint32_t seq = c.nextSeq();
  msg.seq = seq;
  uint8_t buf[oven_ProfilePut_size];
  size_t len = 0;
  TEST_ASSERT_TRUE(protocol::encode(fields, &msg, buf, sizeof(buf), len));
  TEST_ASSERT_TRUE(c.send(type, buf, len, seq));
  return seq;
}

static int stateInt(RequestClient::State s) {
  return static_cast<int>(s);
}

void setUp(void) {}
void tearDown(void) {}

// A ProfileListReq is answered with a ProfileList carrying the summaries, echoing the seq.
void test_list_happy_path(void) {
  Rig r;
  oven_ProfileListReq req = oven_ProfileListReq_init_zero;
  req.mode = oven_Mode_MODE_REFLOW;
  uint32_t seq =
      sendReq(r.client, oven_ProfileListReq_fields, protocol::kTfTypeProfileListReq, req);

  r.pumpCtrl(); // controller receives + replies
  r.pumpCyd();  // CYD receives the reply

  TEST_ASSERT_EQUAL_INT(stateInt(RequestClient::State::Done), stateInt(r.client.state()));
  TEST_ASSERT_EQUAL_INT(1, r.responder.lists);
  TEST_ASSERT_EQUAL_INT(1, r.cyd_obs.lists);
  TEST_ASSERT_EQUAL_UINT32(seq, r.cyd_obs.last_list.seq);
  TEST_ASSERT_EQUAL_UINT32(2, r.cyd_obs.last_list.profiles_count);
  TEST_ASSERT_EQUAL_STRING("LF-245", r.cyd_obs.last_list.profiles[0].name);
  TEST_ASSERT_TRUE(r.cyd_obs.last_list.profiles[0].stock);
}

// A ProfileGetReq is answered with the full ProfileData.
void test_get_happy_path(void) {
  Rig r;
  oven_ProfileGetReq req = oven_ProfileGetReq_init_zero;
  req.mode = oven_Mode_MODE_REFLOW;
  std::strncpy(req.name, "LF-245", sizeof(req.name) - 1);
  uint32_t seq = sendReq(r.client, oven_ProfileGetReq_fields, protocol::kTfTypeProfileGetReq, req);

  r.exchange();

  TEST_ASSERT_EQUAL_INT(stateInt(RequestClient::State::Done), stateInt(r.client.state()));
  TEST_ASSERT_EQUAL_INT(1, r.cyd_obs.datas);
  TEST_ASSERT_EQUAL_UINT32(seq, r.cyd_obs.last_data.seq);
  TEST_ASSERT_TRUE(r.cyd_obs.last_data.has_profile);
  TEST_ASSERT_EQUAL_STRING("LF-245", r.cyd_obs.last_data.profile.name);
  TEST_ASSERT_EQUAL_UINT32(1, r.cyd_obs.last_data.profile.phases_count);
}

// A ProfilePut gets a MgmtResult{ok=true} verdict.
void test_put_verdict(void) {
  Rig r;
  oven_ProfilePut req = oven_ProfilePut_init_zero;
  req.has_profile = true;
  req.profile.mode = oven_Mode_MODE_CURE;
  std::strncpy(req.profile.name, "Resin-A", sizeof(req.profile.name) - 1);
  req.profile.phases_count = 1;
  req.profile.phases[0].target_c = 80.0F;
  uint32_t seq = sendReq(r.client, oven_ProfilePut_fields, protocol::kTfTypeProfilePut, req);

  r.exchange();

  TEST_ASSERT_EQUAL_INT(stateInt(RequestClient::State::Done), stateInt(r.client.state()));
  TEST_ASSERT_EQUAL_INT(1, r.responder.puts);
  TEST_ASSERT_EQUAL_INT(1, r.cyd_obs.results);
  TEST_ASSERT_EQUAL_UINT32(seq, r.cyd_obs.last_result.seq);
  TEST_ASSERT_TRUE(r.cyd_obs.last_result.ok);
}

// Settings get round-trips a SettingsData.
void test_settings_get(void) {
  Rig r;
  oven_SettingsGetReq req = oven_SettingsGetReq_init_zero;
  sendReq(r.client, oven_SettingsGetReq_fields, protocol::kTfTypeSettingsGetReq, req);
  r.exchange();
  TEST_ASSERT_EQUAL_INT(stateInt(RequestClient::State::Done), stateInt(r.client.state()));
  TEST_ASSERT_EQUAL_INT(1, r.cyd_obs.settings);
  TEST_ASSERT_TRUE(r.cyd_obs.last_settings.has_settings);
  TEST_ASSERT_EQUAL_INT(250, r.cyd_obs.last_settings.settings.reflow_max_cap);
}

// One outstanding request at a time: a second send() while Pending is refused.
void test_single_outstanding(void) {
  Rig r;
  oven_ProfileListReq req = oven_ProfileListReq_init_zero;
  uint32_t seq = r.client.nextSeq();
  req.seq = seq;
  uint8_t buf[oven_ProfilePut_size];
  size_t len = 0;
  TEST_ASSERT_TRUE(protocol::encode(oven_ProfileListReq_fields, &req, buf, sizeof(buf), len));
  TEST_ASSERT_TRUE(r.client.send(protocol::kTfTypeProfileListReq, buf, len, seq));
  // second send while Pending -> refused
  TEST_ASSERT_FALSE(r.client.send(protocol::kTfTypeProfileListReq, buf, len, seq));
}

// A dropped request is resent on timeout and then resolves.
void test_retry_dropped_request(void) {
  Rig r;
  r.cyd_tx.drop_tx = true; // the initial send is lost on the wire
  oven_ProfileListReq req = oven_ProfileListReq_init_zero;
  sendReq(r.client, oven_ProfileListReq_fields, protocol::kTfTypeProfileListReq, req);
  r.exchange();
  TEST_ASSERT_EQUAL_INT(stateInt(RequestClient::State::Pending), stateInt(r.client.state()));
  TEST_ASSERT_EQUAL_INT(0, r.responder.lists); // controller never heard the first one

  r.cyd_tx.drop_tx = false; // let the retransmit through
  r.clk.advance(protocol::kSetupAckTimeoutMs);
  r.client.service(); // resend
  r.exchange();

  TEST_ASSERT_EQUAL_INT(stateInt(RequestClient::State::Done), stateInt(r.client.state()));
  TEST_ASSERT_EQUAL_INT(1, r.responder.lists);
  TEST_ASSERT_TRUE(r.client.attempts() >= 1);
}

// A dropped REPLY makes the client resend the request; the responder must dedup — replay the
// cached reply, NOT re-apply the side effect. puts stays 1.
void test_dedup_dropped_reply(void) {
  Rig r;
  oven_ProfilePut req = oven_ProfilePut_init_zero;
  req.has_profile = true;
  req.profile.mode = oven_Mode_MODE_CURE;
  std::strncpy(req.profile.name, "Resin-A", sizeof(req.profile.name) - 1);
  req.profile.phases_count = 1;

  r.ctrl_tx.drop_tx = true; // the controller's reply is lost
  sendReq(r.client, oven_ProfilePut_fields, protocol::kTfTypeProfilePut, req);
  r.exchange();
  TEST_ASSERT_EQUAL_INT(1, r.responder.puts); // side effect applied once
  TEST_ASSERT_EQUAL_INT(stateInt(RequestClient::State::Pending), stateInt(r.client.state()));

  r.ctrl_tx.drop_tx = false; // let the replayed reply through this time
  r.clk.advance(protocol::kSetupAckTimeoutMs);
  r.client.service(); // resend the request
  r.exchange();

  TEST_ASSERT_EQUAL_INT(stateInt(RequestClient::State::Done), stateInt(r.client.state()));
  TEST_ASSERT_EQUAL_INT(1, r.responder.puts); // NOT re-applied — deduped
  TEST_ASSERT_EQUAL_INT(1, r.cyd_obs.results);
}

// No reply ever arrives -> Failed after the retry budget, no side effect.
void test_timeout_to_failed(void) {
  Rig r;
  r.cyd_tx.drop_tx = true; // every send (initial + retries) is lost
  oven_ProfileListReq req = oven_ProfileListReq_init_zero;
  sendReq(r.client, oven_ProfileListReq_fields, protocol::kTfTypeProfileListReq, req);

  for (int i = 0; i < protocol::kSetupMaxRetries + 1; ++i) {
    r.clk.advance(protocol::kSetupAckTimeoutMs);
    r.client.service();
  }
  r.exchange();

  TEST_ASSERT_EQUAL_INT(stateInt(RequestClient::State::Failed), stateInt(r.client.state()));
  TEST_ASSERT_EQUAL_INT(0, r.responder.lists);
}

// clear() returns a terminal client to Idle so the next request can go; seqs stay monotonic.
void test_clear_frees_for_next(void) {
  Rig r;
  oven_ProfileListReq req = oven_ProfileListReq_init_zero;
  uint32_t s1 = sendReq(r.client, oven_ProfileListReq_fields, protocol::kTfTypeProfileListReq, req);
  r.exchange();
  TEST_ASSERT_EQUAL_INT(stateInt(RequestClient::State::Done), stateInt(r.client.state()));

  r.client.clear();
  TEST_ASSERT_TRUE(r.client.idle());

  oven_SettingsGetReq req2 = oven_SettingsGetReq_init_zero;
  uint32_t s2 =
      sendReq(r.client, oven_SettingsGetReq_fields, protocol::kTfTypeSettingsGetReq, req2);
  TEST_ASSERT_TRUE(s2 > s1);
  r.exchange();
  TEST_ASSERT_EQUAL_INT(1, r.cyd_obs.settings);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_list_happy_path);
  RUN_TEST(test_get_happy_path);
  RUN_TEST(test_put_verdict);
  RUN_TEST(test_settings_get);
  RUN_TEST(test_single_outstanding);
  RUN_TEST(test_retry_dropped_request);
  RUN_TEST(test_dedup_dropped_reply);
  RUN_TEST(test_timeout_to_failed);
  RUN_TEST(test_clear_frees_for_next);
  return UNITY_END();
}
