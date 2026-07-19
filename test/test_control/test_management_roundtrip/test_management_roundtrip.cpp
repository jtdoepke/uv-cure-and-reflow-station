// native_control suite — the full profile/settings management round-trip (design.md §9; Wave R3 of
// the §2 "CYD is a UI remote" split, 2026-07-17). Wires the REAL CYD-side ManagementClient
// (lib/app_logic) to the REAL controller-side ManagementResponder + stores (lib/control_logic) over
// a LoopbackPipe — the end-to-end contract the library/editor/settings screens rely on. It is a
// cross-boundary *integration* test (both ends of the wire), so it deliberately pulls app_logic and
// control_logic into one process; the per-side unit tests live in their own lanes.
#include <unity.h>

#include <cstring>

#include "controller_link.h" // controller reliability facade — the real routing seam (§11)
#include "device_settings.h"
#include "frame_link.h"
#include "helpers/fake_clock.h"
#include "helpers/fake_profile_storage.h"
#include "helpers/fake_settings_storage.h"
#include "helpers/pipe_transport.h"
#include "management_client.h"    // CYD
#include "management_responder.h" // controller
#include "message_router.h"
#include "oven.pb.h"
#include "phase.h"
#include "phase_codec.h"
#include "profile_library.h"

// The controller side routes through ControllerLink (setManagementResponder), NOT straight into the
// responder — exactly as src_control/main.cpp wires it. Doing so here is load-bearing: a management
// request only reaches the responder if ControllerLink forwards its virtual, and a missing forward
// (settings once shipped that way) makes the request silently unanswered. Bypassing ControllerLink,
// as an earlier version did, hid that gap; this rig is the firmware object graph (§11).
struct Rig {
  LoopbackPipe pipe;
  FakeClock clk;
  FakeProfileStorage cure_fs;
  FakeProfileStorage reflow_fs;
  FakeSettingsStorage settings_fs;
  control::ProfileStore cure_store;
  control::ProfileStore reflow_store;
  control::SettingsStore settings_store;
  protocol::MessageRouter ctrl_router;
  protocol::FrameLink ctrl_link;
  ManagementResponder responder;
  ControllerLink ctrl;
  protocol::MessageRouter cyd_router;
  protocol::FrameLink cyd_link;
  ManagementClient client;

  Rig()
      : cure_store(cure_fs, oven_Mode_MODE_CURE), reflow_store(reflow_fs, oven_Mode_MODE_REFLOW),
        settings_store(settings_fs), ctrl_link(pipe.b(), TF_SLAVE, ctrl_router),
        responder(ctrl_link, cure_store, reflow_store), ctrl(ctrl_link, clk),
        cyd_link(pipe.a(), TF_MASTER, cyd_router), client(cyd_link, clk) {
    responder.setSettingsStore(settings_store);
    ctrl.setManagementResponder(responder);
    ctrl_router.setObserver(ctrl); // route through the facade, like the firmware
    cyd_router.setObserver(client);
  }

  void exchange() {
    ctrl_link.poll();
    cyd_link.poll();
  }
};

static oven_Profile authoredReflow(const char *name) {
  Phase phases[3];
  std::strncpy(phases[0].name, "Preheat", kPhaseNameCap - 1);
  phases[0].targetC = 150.0F;
  phases[0].rampSeconds = 90.0F;
  std::strncpy(phases[1].name, "Soak", kPhaseNameCap - 1);
  phases[1].targetC = 180.0F;
  std::strncpy(phases[2].name, "Reflow", kPhaseNameCap - 1);
  phases[2].targetC = 245.0F;
  phases[2].holdSeconds = 30.0F;
  return phase_codec::profileToWire(name, RecipeMode::Reflow, false, phases, 3);
}

static bool busy(ManagementClient::State s) {
  return s == ManagementClient::State::Busy;
}

void setUp(void) {}
void tearDown(void) {}

// Editor Save -> the controller stores it; then the library list shows it, one round-trip.
void test_put_then_list(void) {
  Rig r;
  TEST_ASSERT_TRUE(r.client.requestPut(authoredReflow("LF-245")));
  TEST_ASSERT_TRUE(busy(r.client.state()));
  r.exchange();
  TEST_ASSERT_TRUE(r.client.ready());
  r.client.clear();

  TEST_ASSERT_TRUE(r.client.requestList(oven_Mode_MODE_REFLOW));
  r.exchange();
  TEST_ASSERT_TRUE(r.client.ready());
  TEST_ASSERT_EQUAL_UINT32(1, r.client.list().profiles_count);
  TEST_ASSERT_EQUAL_STRING("LF-245", r.client.list().profiles[0].name);
  TEST_ASSERT_EQUAL_FLOAT(245.0F, r.client.list().profiles[0].peak_c);
}

// Get returns the full profile; the codec decodes it back to domain phases.
void test_get_decodes_phases(void) {
  Rig r;
  r.client.requestPut(authoredReflow("LF-245"));
  r.exchange();
  r.client.clear();

  TEST_ASSERT_TRUE(r.client.requestGet(oven_Mode_MODE_REFLOW, "LF-245"));
  r.exchange();
  TEST_ASSERT_TRUE(r.client.ready());
  TEST_ASSERT_EQUAL_STRING("LF-245", r.client.profile().name);
  Phase back[kMaxPhases];
  size_t n = phase_codec::phasesFromWire(r.client.profile(), back, kMaxPhases);
  TEST_ASSERT_EQUAL_UINT32(3, n);
  TEST_ASSERT_EQUAL_STRING("Reflow", back[2].name);
  TEST_ASSERT_EQUAL_FLOAT(245.0F, back[2].targetC);
}

// A get for an absent profile fails with NOT_FOUND (the screen's error state).
void test_get_absent_fails(void) {
  Rig r;
  TEST_ASSERT_TRUE(r.client.requestGet(oven_Mode_MODE_REFLOW, "nope"));
  r.exchange();
  TEST_ASSERT_TRUE(r.client.failed());
  TEST_ASSERT_EQUAL_INT(oven_NakReason_NAK_NOT_FOUND, r.client.lastNak());
}

// Deleting a stock profile fails with STOCK_READONLY (§23 gating, surfaced to the CYD).
void test_delete_stock_fails(void) {
  Rig r;
  oven_Profile stock = authoredReflow("SAC305");
  stock.stock = true;
  TEST_ASSERT_TRUE(r.reflow_store.save(stock));

  TEST_ASSERT_TRUE(r.client.requestDelete(oven_Mode_MODE_REFLOW, "SAC305"));
  r.exchange();
  TEST_ASSERT_TRUE(r.client.failed());
  TEST_ASSERT_EQUAL_INT(oven_NakReason_NAK_STOCK_READONLY, r.client.lastNak());
}

// Duplicate (CYD pre-deconflicts the name) creates a user copy.
void test_duplicate(void) {
  Rig r;
  r.client.requestPut(authoredReflow("LF-245"));
  r.exchange();
  r.client.clear();

  TEST_ASSERT_TRUE(r.client.requestDup(oven_Mode_MODE_REFLOW, "LF-245", "LF-245 copy"));
  r.exchange();
  TEST_ASSERT_TRUE(r.client.ready());
  TEST_ASSERT_TRUE(r.reflow_store.contains("LF-245 copy"));
}

// Settings get returns the controller's defaults; put persists a change.
void test_settings_roundtrip(void) {
  Rig r;
  TEST_ASSERT_TRUE(r.client.requestSettingsGet());
  r.exchange();
  TEST_ASSERT_TRUE(r.client.ready());
  TEST_ASSERT_EQUAL_INT(250, r.client.settings().reflow_max_cap);
  oven_Settings s = r.client.settings();
  r.client.clear();

  s.reflow_max_cap = 240;
  TEST_ASSERT_TRUE(r.client.requestSettingsPut(s));
  r.exchange();
  TEST_ASSERT_TRUE(r.client.ready());
  TEST_ASSERT_EQUAL_INT(240, r.settings_store.get().reflow_max_cap);
}

// A ProfileTouch round-trips and bumps the profile's recency; a subsequent MRU-sorted list reflects
// it (§23). The controller sorts, so the picker's order is observed through requestList(..., MRU).
void test_touch_roundtrip_reorders_mru(void) {
  Rig r;
  r.client.requestPut(authoredReflow("A"));
  r.exchange();
  r.client.clear();
  r.client.requestPut(authoredReflow("B")); // saved later -> more recent than A
  r.exchange();
  r.client.clear();

  // Before the touch: MRU order is B, A (B saved most recently).
  TEST_ASSERT_TRUE(r.client.requestList(oven_Mode_MODE_REFLOW, oven_ProfileSort_PROFILE_SORT_MRU));
  r.exchange();
  TEST_ASSERT_TRUE(r.client.ready());
  TEST_ASSERT_EQUAL_UINT32(2, r.client.list().profiles_count);
  TEST_ASSERT_EQUAL_STRING("B", r.client.list().profiles[0].name);
  TEST_ASSERT_EQUAL_STRING("A", r.client.list().profiles[1].name);
  r.client.clear();

  TEST_ASSERT_TRUE(r.client.requestTouch(oven_Mode_MODE_REFLOW, "A"));
  r.exchange();
  TEST_ASSERT_TRUE(r.client.ready()); // MgmtResult ok (the CYD ignores it, but it must succeed)
  r.client.clear();

  // After touching A: MRU order flips to A, B. Alpha order is unaffected (A, B).
  TEST_ASSERT_TRUE(r.client.requestList(oven_Mode_MODE_REFLOW, oven_ProfileSort_PROFILE_SORT_MRU));
  r.exchange();
  TEST_ASSERT_EQUAL_STRING("A", r.client.list().profiles[0].name);
  TEST_ASSERT_EQUAL_STRING("B", r.client.list().profiles[1].name);
  r.client.clear();
  TEST_ASSERT_TRUE(
      r.client.requestList(oven_Mode_MODE_REFLOW, oven_ProfileSort_PROFILE_SORT_ALPHA));
  r.exchange();
  TEST_ASSERT_EQUAL_STRING("A", r.client.list().profiles[0].name);
  TEST_ASSERT_EQUAL_STRING("B", r.client.list().profiles[1].name);
}

// A touch of an absent profile fails (NOT_FOUND) — the client would see Failed, and ignores it.
void test_touch_absent_fails(void) {
  Rig r;
  TEST_ASSERT_TRUE(r.client.requestTouch(oven_Mode_MODE_REFLOW, "ghost"));
  r.exchange();
  TEST_ASSERT_TRUE(r.client.failed());
}

// One outstanding request at a time (single-outstanding, like the setup path).
void test_single_outstanding(void) {
  Rig r;
  TEST_ASSERT_TRUE(r.client.requestList(oven_Mode_MODE_REFLOW));
  TEST_ASSERT_FALSE(r.client.requestList(oven_Mode_MODE_CURE)); // refused while Busy
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_put_then_list);
  RUN_TEST(test_get_decodes_phases);
  RUN_TEST(test_get_absent_fails);
  RUN_TEST(test_delete_stock_fails);
  RUN_TEST(test_duplicate);
  RUN_TEST(test_settings_roundtrip);
  RUN_TEST(test_touch_roundtrip_reorders_mru);
  RUN_TEST(test_touch_absent_fails);
  RUN_TEST(test_single_outstanding);
  return UNITY_END();
}
