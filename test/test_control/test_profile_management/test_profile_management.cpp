// native_control suite — the controller-side profile library (design.md §7/§23) and the profile
// management round-trip (§9), added 2026-07-17 with the §2 "CYD is a UI remote" split (Wave R2).
// Two halves: (1) control::ProfileStore over a FakeProfileStorage — save/load/list/dup/rename/
// delete + the stock rule + untrusted-blob rejection; (2) a real ManagementResponder answering a
// real RequestClient over a LoopbackPipe — list/get/put/delete verdicts, NAK reasons, and the
// dropped-reply dedup that must not double-apply a write.
#include <unity.h>

#include <cstring>

#include "codec.h"
#include "device_settings.h"
#include "frame_link.h"
#include "helpers/fake_clock.h"
#include "helpers/fake_profile_storage.h"
#include "helpers/fake_settings_storage.h"
#include "helpers/flaky_transport.h"
#include "helpers/pipe_transport.h"
#include "link_params.h"
#include "management_responder.h"
#include "message_router.h"
#include "messages.h"
#include "oven.pb.h"
#include "profile_library.h"
#include "request_client.h"

using control::ProfileStore;
using protocol::RequestClient;

static oven_Profile makeProfile(oven_Mode mode, const char *name, float peak, uint32_t hold_s) {
  oven_Profile p = oven_Profile_init_zero;
  p.mode = mode;
  std::strncpy(p.name, name, sizeof(p.name) - 1);
  p.phases_count = 2;
  p.phases[0].target_c = peak * 0.5F;
  p.phases[0].ramp_s = 60;
  p.phases[0].fan_mode = oven_FanMode_FAN_MODE_AUTO;
  p.phases[1].target_c = peak;
  p.phases[1].ramp_s = 60;
  p.phases[1].hold_s = static_cast<float>(hold_s);
  return p;
}

void setUp(void) {}
void tearDown(void) {}

// ---- (1) store unit ---------------------------------------------------------------------------

void test_store_save_load_roundtrip(void) {
  FakeProfileStorage fs;
  ProfileStore store(fs, oven_Mode_MODE_REFLOW);
  TEST_ASSERT_TRUE(store.save(makeProfile(oven_Mode_MODE_REFLOW, "LF-245", 245.0F, 90)));

  oven_Profile got = oven_Profile_init_zero;
  TEST_ASSERT_TRUE(store.load("LF-245", got));
  TEST_ASSERT_EQUAL_STRING("LF-245", got.name);
  TEST_ASSERT_EQUAL_INT(oven_Mode_MODE_REFLOW, got.mode);
  TEST_ASSERT_EQUAL_UINT32(2, got.phases_count);
  TEST_ASSERT_EQUAL_FLOAT(245.0F, got.phases[1].target_c);
}

// The store stamps its OWN mode and rejects a foreign-mode blob on load (§7 never-mixed).
void test_store_mode_guard(void) {
  FakeProfileStorage fs;
  ProfileStore reflow(fs, oven_Mode_MODE_REFLOW);
  // Save a CURE-tagged profile through the reflow store: it gets stamped REFLOW.
  TEST_ASSERT_TRUE(reflow.save(makeProfile(oven_Mode_MODE_CURE, "X", 200.0F, 10)));
  oven_Profile got = oven_Profile_init_zero;
  TEST_ASSERT_TRUE(reflow.load("X", got));
  TEST_ASSERT_EQUAL_INT(oven_Mode_MODE_REFLOW, got.mode);
  // A CURE store over the SAME storage refuses that blob (wrong mode).
  ProfileStore cure(fs, oven_Mode_MODE_CURE);
  TEST_ASSERT_FALSE(cure.load("X", got));
  ProfileStore::Summary rows[ProfileStore::kMaxListed];
  TEST_ASSERT_EQUAL_UINT32(0, cure.list(rows, ProfileStore::kMaxListed));
}

void test_store_stock_readonly(void) {
  FakeProfileStorage fs;
  ProfileStore store(fs, oven_Mode_MODE_CURE);
  oven_Profile stock = makeProfile(oven_Mode_MODE_CURE, "Resin-A", 80.0F, 300);
  stock.stock = true;
  TEST_ASSERT_TRUE(store.save(stock));

  // save-over, delete, rename all refused; duplicate is allowed (and clears stock).
  TEST_ASSERT_FALSE(store.save(makeProfile(oven_Mode_MODE_CURE, "Resin-A", 80.0F, 111)));
  TEST_ASSERT_FALSE(store.remove("Resin-A"));
  TEST_ASSERT_FALSE(store.rename("Resin-A", "Resin-B"));
  TEST_ASSERT_TRUE(store.duplicate("Resin-A", "Resin-A copy"));
  oven_Profile copy = oven_Profile_init_zero;
  TEST_ASSERT_TRUE(store.load("Resin-A copy", copy));
  TEST_ASSERT_FALSE(copy.stock);
}

void test_store_list_sorted_facts(void) {
  FakeProfileStorage fs;
  ProfileStore store(fs, oven_Mode_MODE_REFLOW);
  TEST_ASSERT_TRUE(store.save(makeProfile(oven_Mode_MODE_REFLOW, "SAC305", 249.0F, 60)));
  TEST_ASSERT_TRUE(store.save(makeProfile(oven_Mode_MODE_REFLOW, "LF-245", 245.0F, 90)));

  ProfileStore::Summary rows[ProfileStore::kMaxListed];
  size_t n = store.list(rows, ProfileStore::kMaxListed);
  TEST_ASSERT_EQUAL_UINT32(2, n);
  TEST_ASSERT_EQUAL_STRING("LF-245", rows[0].name); // alphabetical
  TEST_ASSERT_EQUAL_STRING("SAC305", rows[1].name);
  TEST_ASSERT_EQUAL_FLOAT(245.0F, rows[0].facts.peak_c);
  TEST_ASSERT_EQUAL_UINT32(60 + 60 + 90, rows[0].facts.total_s); // ramp+ramp+hold
}

void test_store_invalid_name_refused(void) {
  FakeProfileStorage fs;
  ProfileStore store(fs, oven_Mode_MODE_REFLOW);
  TEST_ASSERT_FALSE(ProfileStore::validName("../etc/passwd"));
  TEST_ASSERT_FALSE(ProfileStore::validName("a/b"));
  TEST_ASSERT_FALSE(ProfileStore::validName(""));
  TEST_ASSERT_TRUE(ProfileStore::validName("LF-245 copy"));
  oven_Profile p = makeProfile(oven_Mode_MODE_REFLOW, "ok", 200.0F, 10);
  std::strncpy(p.name, "bad/name", sizeof(p.name) - 1);
  TEST_ASSERT_FALSE(store.save(p));
}

// An untrusted flash blob: bad header and a truncated body are both rejected, never mis-parsed.
void test_store_untrusted_blob_rejected(void) {
  FakeProfileStorage fs;
  ProfileStore store(fs, oven_Mode_MODE_REFLOW);

  // garbage bytes (bad magic)
  std::vector<uint8_t> junk(40, 0xAB);
  fs.put("junk", junk);
  oven_Profile got = oven_Profile_init_zero;
  TEST_ASSERT_FALSE(store.load("junk", got));

  // a valid blob truncated mid-body -> decode fails
  oven_Profile p = makeProfile(oven_Mode_MODE_REFLOW, "T", 245.0F, 90);
  uint8_t buf[ProfileStore::kBlobCap];
  size_t len = 0;
  TEST_ASSERT_TRUE(ProfileStore::encodeBlob(p, buf, sizeof(buf), len));
  std::vector<uint8_t> truncated(buf, buf + (len - 3));
  fs.put("T", truncated);
  TEST_ASSERT_FALSE(store.load("T", got));

  // list() skips both bad blobs
  ProfileStore::Summary rows[ProfileStore::kMaxListed];
  TEST_ASSERT_EQUAL_UINT32(0, store.list(rows, ProfileStore::kMaxListed));
}

// Find a list row by name; returns its use_seq (0 if absent). Small helper for the MRU tests.
static uint32_t seqOf(const ProfileStore::Summary *rows, size_t n, const char *name) {
  for (size_t i = 0; i < n; ++i) {
    if (std::strcmp(rows[i].name, name) == 0) {
      return rows[i].use_seq;
    }
  }
  return 0;
}

// save() stamps a controller-owned recency counter that strictly increases per write, so a
// later-saved profile always outranks an earlier one (§23 MRU). A wire-supplied use_seq is ignored.
void test_store_use_seq_stamped_monotonic(void) {
  FakeProfileStorage fs;
  ProfileStore store(fs, oven_Mode_MODE_REFLOW);
  oven_Profile a = makeProfile(oven_Mode_MODE_REFLOW, "A", 200.0F, 10);
  a.use_seq = 999; // the store must NOT trust this
  TEST_ASSERT_TRUE(store.save(a));
  TEST_ASSERT_TRUE(store.save(makeProfile(oven_Mode_MODE_REFLOW, "B", 200.0F, 10)));

  ProfileStore::Summary rows[ProfileStore::kMaxListed];
  size_t n = store.list(rows, ProfileStore::kMaxListed);
  TEST_ASSERT_EQUAL_UINT32(2, n);
  const uint32_t sa = seqOf(rows, n, "A");
  const uint32_t sb = seqOf(rows, n, "B");
  TEST_ASSERT_NOT_EQUAL(999, sa);          // wire value discarded
  TEST_ASSERT_GREATER_THAN_UINT32(0, sa);  // stamped
  TEST_ASSERT_GREATER_THAN_UINT32(sa, sb); // B saved later -> higher rank
}

// touch() bumps a profile's recency above every other — the run-start "mark used" (§23). It is
// allowed on stock (running a stock profile is a use) and fails only for an absent name.
void test_store_touch_bumps_recency(void) {
  FakeProfileStorage fs;
  ProfileStore store(fs, oven_Mode_MODE_CURE);
  oven_Profile stock = makeProfile(oven_Mode_MODE_CURE, "Resin-A", 80.0F, 300);
  stock.stock = true;
  TEST_ASSERT_TRUE(store.save(stock));
  TEST_ASSERT_TRUE(store.save(makeProfile(oven_Mode_MODE_CURE, "Resin-B", 80.0F, 300)));

  ProfileStore::Summary rows[ProfileStore::kMaxListed];
  size_t n = store.list(rows, ProfileStore::kMaxListed);
  TEST_ASSERT_GREATER_THAN_UINT32(seqOf(rows, n, "Resin-A"), seqOf(rows, n, "Resin-B"));

  // Touch the older, stock profile: it now outranks B, and stays stock + unchanged in content.
  TEST_ASSERT_TRUE(store.touch("Resin-A"));
  n = store.list(rows, ProfileStore::kMaxListed);
  TEST_ASSERT_GREATER_THAN_UINT32(seqOf(rows, n, "Resin-B"), seqOf(rows, n, "Resin-A"));
  oven_Profile got = oven_Profile_init_zero;
  TEST_ASSERT_TRUE(store.load("Resin-A", got));
  TEST_ASSERT_TRUE(got.stock);
  TEST_ASSERT_EQUAL_FLOAT(80.0F, got.phases[1].target_c);

  TEST_ASSERT_FALSE(store.touch("nope")); // absent
}

// list(Mru) orders newest-first by the recency counter; list(Alpha) stays by name. The controller
// owns the ordering (§23) — the CYD just renders what it asked for.
void test_store_list_mru_order(void) {
  FakeProfileStorage fs;
  ProfileStore store(fs, oven_Mode_MODE_REFLOW);
  TEST_ASSERT_TRUE(store.save(makeProfile(oven_Mode_MODE_REFLOW, "Aaa", 200.0F, 10)));
  TEST_ASSERT_TRUE(store.save(makeProfile(oven_Mode_MODE_REFLOW, "Bbb", 200.0F, 10)));
  TEST_ASSERT_TRUE(store.save(makeProfile(oven_Mode_MODE_REFLOW, "Ccc", 200.0F, 10)));
  TEST_ASSERT_TRUE(store.touch("Aaa")); // Aaa is now the most-recently-used

  ProfileStore::Summary rows[ProfileStore::kMaxListed];
  // Alpha: by name.
  size_t n = store.list(rows, ProfileStore::kMaxListed, ProfileStore::SortMode::Alpha);
  TEST_ASSERT_EQUAL_UINT32(3, n);
  TEST_ASSERT_EQUAL_STRING("Aaa", rows[0].name);
  TEST_ASSERT_EQUAL_STRING("Bbb", rows[1].name);
  TEST_ASSERT_EQUAL_STRING("Ccc", rows[2].name);
  // Mru: Aaa (just touched) first, then Ccc, Bbb (saved newest-first).
  n = store.list(rows, ProfileStore::kMaxListed, ProfileStore::SortMode::Mru);
  TEST_ASSERT_EQUAL_STRING("Aaa", rows[0].name);
  TEST_ASSERT_EQUAL_STRING("Ccc", rows[1].name);
  TEST_ASSERT_EQUAL_STRING("Bbb", rows[2].name);
}

// Under Mru, profiles with equal/unset use_seq fall back to name ascending (deterministic).
void test_store_list_mru_ties_by_name(void) {
  FakeProfileStorage fs;
  ProfileStore store(fs, oven_Mode_MODE_CURE);
  // Seed three stock profiles directly (encodeBlob bypasses save()'s stamping, so use_seq stays 0).
  for (const char *nm : {"Resin-C", "Resin-A", "Resin-B"}) {
    oven_Profile p = makeProfile(oven_Mode_MODE_CURE, nm, 80.0F, 60);
    p.stock = true;
    uint8_t buf[ProfileStore::kBlobCap];
    size_t len = 0;
    TEST_ASSERT_TRUE(ProfileStore::encodeBlob(p, buf, sizeof(buf), len));
    fs.put(nm, std::vector<uint8_t>(buf, buf + len));
  }
  ProfileStore::Summary rows[ProfileStore::kMaxListed];
  size_t n = store.list(rows, ProfileStore::kMaxListed, ProfileStore::SortMode::Mru);
  TEST_ASSERT_EQUAL_UINT32(3, n);
  TEST_ASSERT_EQUAL_STRING("Resin-A", rows[0].name);
  TEST_ASSERT_EQUAL_STRING("Resin-B", rows[1].name);
  TEST_ASSERT_EQUAL_STRING("Resin-C", rows[2].name);
}

// ---- (2) management round-trip (real ManagementResponder <-> real RequestClient) --------------

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
  FlakyTransport cyd_tx;
  FlakyTransport ctrl_tx;
  FakeClock clk;
  FakeProfileStorage cure_fs;
  FakeProfileStorage reflow_fs;
  FakeSettingsStorage settings_fs;
  ProfileStore cure_store;
  ProfileStore reflow_store;
  control::SettingsStore settings_store;
  protocol::MessageRouter ctrl_router;
  protocol::FrameLink ctrl_link;
  ManagementResponder responder;
  protocol::MessageRouter cyd_router;
  protocol::FrameLink cyd_link;
  RequestClient client;
  CydObserver cyd_obs;

  Rig()
      : cyd_tx(pipe.a()), ctrl_tx(pipe.b()), cure_store(cure_fs, oven_Mode_MODE_CURE),
        reflow_store(reflow_fs, oven_Mode_MODE_REFLOW), settings_store(settings_fs), ctrl_router(),
        ctrl_link(ctrl_tx, TF_SLAVE, ctrl_router), responder(ctrl_link, cure_store, reflow_store),
        cyd_router(), cyd_link(cyd_tx, TF_MASTER, cyd_router), client(cyd_link, clk),
        cyd_obs(client) {
    responder.setSettingsStore(settings_store);
    ctrl_router.setObserver(responder);
    cyd_router.setObserver(cyd_obs);
  }
  void exchange() {
    ctrl_link.poll();
    cyd_link.poll();
  }
};

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

// Put a profile, then list it back over the wire.
void test_mgmt_put_then_list(void) {
  Rig r;
  oven_ProfilePut put = oven_ProfilePut_init_zero;
  put.has_profile = true;
  put.profile = makeProfile(oven_Mode_MODE_REFLOW, "LF-245", 245.0F, 90);
  sendReq(r.client, oven_ProfilePut_fields, protocol::kTfTypeProfilePut, put);
  r.exchange();
  TEST_ASSERT_TRUE(r.cyd_obs.last_result.ok);
  r.client.clear();

  oven_ProfileListReq lst = oven_ProfileListReq_init_zero;
  lst.mode = oven_Mode_MODE_REFLOW;
  sendReq(r.client, oven_ProfileListReq_fields, protocol::kTfTypeProfileListReq, lst);
  r.exchange();
  TEST_ASSERT_EQUAL_UINT32(1, r.cyd_obs.last_list.profiles_count);
  TEST_ASSERT_EQUAL_STRING("LF-245", r.cyd_obs.last_list.profiles[0].name);
  TEST_ASSERT_EQUAL_FLOAT(245.0F, r.cyd_obs.last_list.profiles[0].peak_c);
}

// Get a stored profile back in full.
void test_mgmt_get(void) {
  Rig r;
  TEST_ASSERT_TRUE(r.reflow_store.save(makeProfile(oven_Mode_MODE_REFLOW, "LF-245", 245.0F, 90)));
  oven_ProfileGetReq req = oven_ProfileGetReq_init_zero;
  req.mode = oven_Mode_MODE_REFLOW;
  std::strncpy(req.name, "LF-245", sizeof(req.name) - 1);
  sendReq(r.client, oven_ProfileGetReq_fields, protocol::kTfTypeProfileGetReq, req);
  r.exchange();
  TEST_ASSERT_EQUAL_INT(1, r.cyd_obs.datas);
  TEST_ASSERT_TRUE(r.cyd_obs.last_data.has_profile);
  TEST_ASSERT_EQUAL_STRING("LF-245", r.cyd_obs.last_data.profile.name);
  TEST_ASSERT_EQUAL_UINT32(2, r.cyd_obs.last_data.profile.phases_count);
}

// A get for an absent profile is a MgmtResult{ok=false, NOT_FOUND}.
void test_mgmt_get_not_found(void) {
  Rig r;
  oven_ProfileGetReq req = oven_ProfileGetReq_init_zero;
  req.mode = oven_Mode_MODE_REFLOW;
  std::strncpy(req.name, "nope", sizeof(req.name) - 1);
  sendReq(r.client, oven_ProfileGetReq_fields, protocol::kTfTypeProfileGetReq, req);
  r.exchange();
  TEST_ASSERT_EQUAL_INT(1, r.cyd_obs.results);
  TEST_ASSERT_FALSE(r.cyd_obs.last_result.ok);
  TEST_ASSERT_EQUAL_INT(oven_NakReason_NAK_NOT_FOUND, r.cyd_obs.last_result.reason);
}

// Deleting a stock profile is refused with STOCK_READONLY over the wire.
void test_mgmt_delete_stock_refused(void) {
  Rig r;
  oven_Profile stock = makeProfile(oven_Mode_MODE_CURE, "Resin-A", 80.0F, 300);
  stock.stock = true;
  TEST_ASSERT_TRUE(r.cure_store.save(stock));

  oven_ProfileDelete del = oven_ProfileDelete_init_zero;
  del.mode = oven_Mode_MODE_CURE;
  std::strncpy(del.name, "Resin-A", sizeof(del.name) - 1);
  sendReq(r.client, oven_ProfileDelete_fields, protocol::kTfTypeProfileDelete, del);
  r.exchange();
  TEST_ASSERT_FALSE(r.cyd_obs.last_result.ok);
  TEST_ASSERT_EQUAL_INT(oven_NakReason_NAK_STOCK_READONLY, r.cyd_obs.last_result.reason);
  TEST_ASSERT_TRUE(r.cure_store.contains("Resin-A")); // still there
}

// A dropped reply makes the client resend the Put; the responder dedups — one write only.
void test_mgmt_dedup_no_double_write(void) {
  Rig r;
  oven_ProfilePut put = oven_ProfilePut_init_zero;
  put.has_profile = true;
  put.profile = makeProfile(oven_Mode_MODE_REFLOW, "Once", 200.0F, 30);

  r.ctrl_tx.drop_tx = true; // lose the first reply
  sendReq(r.client, oven_ProfilePut_fields, protocol::kTfTypeProfilePut, put);
  r.exchange();
  TEST_ASSERT_EQUAL_INT(1, r.reflow_fs.writeCalls); // written once
  TEST_ASSERT_TRUE(r.client.busy());                // reply lost -> still Pending

  r.ctrl_tx.drop_tx = false;
  r.clk.advance(protocol::kSetupAckTimeoutMs);
  r.client.service();
  r.exchange();
  TEST_ASSERT_TRUE(r.cyd_obs.last_result.ok);
  TEST_ASSERT_EQUAL_INT(1, r.reflow_fs.writeCalls); // NOT written twice — deduped
}

// Settings get returns the controller's current settings (defaults on a fresh store).
void test_mgmt_settings_get(void) {
  Rig r;
  oven_SettingsGetReq req = oven_SettingsGetReq_init_zero;
  sendReq(r.client, oven_SettingsGetReq_fields, protocol::kTfTypeSettingsGetReq, req);
  r.exchange();
  TEST_ASSERT_EQUAL_INT(1, r.cyd_obs.settings);
  TEST_ASSERT_TRUE(r.cyd_obs.last_settings.has_settings);
  TEST_ASSERT_EQUAL_INT(250, r.cyd_obs.last_settings.settings.reflow_max_cap);
  TEST_ASSERT_EQUAL_INT(100, r.cyd_obs.last_settings.settings.uv_max_cap);
}

// Settings put persists; a cap above the controller's hard-max is clamped (§4 defense-in-depth).
void test_mgmt_settings_put_clamps_cap(void) {
  Rig r;
  oven_SettingsPut put = oven_SettingsPut_init_zero;
  put.has_settings = true;
  put.settings = control::defaultSettings();
  put.settings.reflow_max_cap = 9999; // above REFLOW_HARD_MAX_C (300)
  put.settings.uv_max_cap = 200;      // above CURE_HARD_MAX_C (120)
  sendReq(r.client, oven_SettingsPut_fields, protocol::kTfTypeSettingsPut, put);
  r.exchange();
  TEST_ASSERT_TRUE(r.cyd_obs.last_result.ok);
  TEST_ASSERT_EQUAL_INT(300, r.settings_store.get().reflow_max_cap);
  TEST_ASSERT_EQUAL_INT(120, r.settings_store.get().uv_max_cap);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_store_save_load_roundtrip);
  RUN_TEST(test_store_mode_guard);
  RUN_TEST(test_store_stock_readonly);
  RUN_TEST(test_store_list_sorted_facts);
  RUN_TEST(test_store_invalid_name_refused);
  RUN_TEST(test_store_untrusted_blob_rejected);
  RUN_TEST(test_store_use_seq_stamped_monotonic);
  RUN_TEST(test_store_touch_bumps_recency);
  RUN_TEST(test_store_list_mru_order);
  RUN_TEST(test_store_list_mru_ties_by_name);
  RUN_TEST(test_mgmt_put_then_list);
  RUN_TEST(test_mgmt_get);
  RUN_TEST(test_mgmt_get_not_found);
  RUN_TEST(test_mgmt_delete_stock_refused);
  RUN_TEST(test_mgmt_dedup_no_double_write);
  RUN_TEST(test_mgmt_settings_get);
  RUN_TEST(test_mgmt_settings_put_clamps_cap);
  return UNITY_END();
}
