// native_ui_cyd suite — the §19 Confirm screen (C6b). Drives the real ConfirmRunScreen against a
// real CydLink/ReliableSender, injecting the controller's Acks + telemetry directly (the over-the-
// wire authorize path is proven by test_bench_link; here the concern is that the SCREEN drives the
// §9 start handshake and the safety gate correctly). Geometry-independent; runs at both sizes.
#include <cstring>
#include <limits>

#include <unity.h>
#include <lvgl.h>
#include "src/debugging/test/lv_test.h" // lv_test_display_create / indev (gated by LV_USE_TEST)

#include "confirm_run_screen.h"
#include "cyd_link.h"
#include "frame_link.h"
#include "helpers/fake_clock.h"
#include "helpers/pipe_transport.h"
#include "management_client.h"
#include "message_router.h"
#include "oven.pb.h"
#include "panel.h"
#include "profile_templates.h"
#include "subjects.h"

using Page = ConfirmRunScreen::Page;

// The CYD link stack. Frames the sender emits go into the pipe and are never drained (the
// controller is simulated by injecting Acks straight into cydlink); telemetry is injected the same
// way.
static LoopbackPipe pipe;
static FakeClock clk;
static protocol::MessageRouter cyd_router;
static protocol::FrameLink cyd_link(pipe.a(), TF_MASTER, cyd_router);
static protocol::CydLink cydlink(cyd_link, clk);
static ManagementClient mgmt(cyd_link, clk);
static ConfirmRunScreen screen;

static constexpr uint32_t kSession = 0xABCD0001;

static bool g_cancelled;
static bool g_committed;
static ProfileDraft g_commit_draft;
static void on_exit(void *) {
  g_cancelled = true;
}
static void on_commit(void *, const ProfileDraft &d) {
  g_committed = true;
  g_commit_draft = d;
}

void setUp(void) {
  lv_init();
  lv_test_display_create(panel::W, panel::H);
  lv_test_indev_create_all();
  ui_subjects_init();
  cyd_router.setObserver(cydlink);
  // The link stack is a static reused across tests — clear the run-authorization state so a prior
  // commit's enabled heartbeat can't leak into the next test.
  cydlink.heartbeat().setEnable(false);
  cydlink.heartbeat().setSession(0);
  screen.setExitHandler(on_exit, nullptr);
  screen.setCommitHandler(on_commit, nullptr);
  g_cancelled = false;
  g_committed = false;
  g_commit_draft = ProfileDraft{};
  lv_subject_set_int(&subj_link_state, LINK_OK);
}
void tearDown(void) {
  lv_deinit();
}

static ProfileDraft draft(RecipeMode mode, const char *name) {
  ProfileDraft d = profile_templates::defaultTemplate(mode);
  std::strncpy(d.name, name, kProfileNameCap - 1);
  return d;
}

// Inject one telemetry frame (as if received over the wire) with the given workpiece temp; walls
// sit at a cool 25 °C.
static void feedTelem(float workC, bool doorOpen = false) {
  oven_Telemetry t = oven_Telemetry_init_zero;
  t.work_temp = workC;
  t.door_open = doorOpen;
  t.wall_temp_count = 4;
  for (size_t i = 0; i < 4; ++i) {
    t.wall_temp[i] = 25.0f;
  }
  cydlink.onTelemetry(t);
}

// Ack whatever setup command is outstanding (the controller's role).
static void ackPending() {
  oven_Ack a = oven_Ack_init_default;
  a.seq = cydlink.sender().pendingSeq();
  cydlink.onAck(a);
}

// Drive the commit handshake to completion (poll → ack → poll → ack → enabled).
static void driveCommit() {
  for (int i = 0; i < 12 && !screen.committed() && screen.page() != Page::Failed; ++i) {
    screen.poll();
    if (cydlink.sender().busy()) {
      ackPending();
    }
  }
}

// Reflow refuses to arm without a plausible workpiece probe, and arms once it reads like the load.
void test_reflow_gate_blocks_without_probe(void) {
  screen.begin(draft(RecipeMode::Reflow, "LF-245"), kSession, cydlink, mgmt);
  screen.render(lv_screen_active());
  screen.poll();
  TEST_ASSERT_FALSE(screen.ready()); // no telemetry yet

  feedTelem(std::numeric_limits<float>::quiet_NaN()); // faulted/open probe
  screen.poll();
  TEST_ASSERT_FALSE(screen.ready());
  screen.commit(); // guarded — nothing starts
  TEST_ASSERT_EQUAL_INT((int)Page::Review, (int)screen.page());

  feedTelem(24.0f); // probe clipped on, reads like the cool chamber
  screen.poll();
  TEST_ASSERT_TRUE(screen.ready());
}

// A reflow commit runs the §9 handshake and enables the run: session adopted, heartbeat enabled,
// the authored draft handed to the Run screen.
void test_reflow_commit_starts_run(void) {
  screen.begin(draft(RecipeMode::Reflow, "LF-245"), kSession, cydlink, mgmt);
  screen.render(lv_screen_active());
  feedTelem(24.0f);
  screen.poll();
  TEST_ASSERT_TRUE(screen.ready());

  screen.commit();
  TEST_ASSERT_EQUAL_INT((int)Page::Starting, (int)screen.page());
  driveCommit();

  TEST_ASSERT_TRUE(screen.committed());
  TEST_ASSERT_TRUE(cydlink.heartbeat().enable());
  TEST_ASSERT_EQUAL_UINT32(kSession, cydlink.heartbeat().session());
  TEST_ASSERT_TRUE(g_committed);
  TEST_ASSERT_EQUAL_STRING("LF-245", g_commit_draft.name);
}

// Cure has no TC gate: with a healthy link it is ready and commits without any telemetry.
void test_cure_needs_no_probe(void) {
  screen.begin(draft(RecipeMode::Cure, "Resin-A"), kSession, cydlink, mgmt);
  screen.render(lv_screen_active());
  // Feed telemetry explicitly: `ready()` now needs a door reading, and CydLink's hasTelemetry()
  // LATCHES, so without this the test would silently depend on an earlier one having fed a frame
  // into the shared link — which is exactly how it passed before the door gate existed.
  feedTelem(24.0f);
  screen.poll();
  TEST_ASSERT_TRUE(screen.ready()); // no probe required

  screen.commit();
  driveCommit();
  TEST_ASSERT_TRUE(screen.committed());
  TEST_ASSERT_TRUE(cydlink.heartbeat().enable());
}

// A Nak on the recipe drops to Failed — the run never enables.
void test_nak_goes_to_failed(void) {
  screen.begin(draft(RecipeMode::Cure, "Resin-A"), kSession, cydlink, mgmt);
  screen.render(lv_screen_active());
  screen.commit();
  screen.poll(); // sends the recipe → Pending

  oven_Nak n = oven_Nak_init_default;
  n.seq = cydlink.sender().pendingSeq();
  n.reason = oven_NakReason_NAK_UNSPECIFIED;
  cydlink.onNak(n);
  screen.poll(); // sees Nakd

  TEST_ASSERT_EQUAL_INT((int)Page::Failed, (int)screen.page());
  TEST_ASSERT_FALSE(screen.committed());
  TEST_ASSERT_FALSE(cydlink.heartbeat().enable());
}

// A dropped link is never ready (even for cure), and Cancel exits to Setup.
void test_link_down_and_cancel(void) {
  screen.begin(draft(RecipeMode::Cure, "Resin-A"), kSession, cydlink, mgmt);
  screen.render(lv_screen_active());
  lv_subject_set_int(&subj_link_state, LINK_NONE);
  screen.poll();
  TEST_ASSERT_FALSE(screen.ready());

  screen.cancel();
  TEST_ASSERT_TRUE(g_cancelled);
}

// The pure TC precondition: NaN / open-circuit / implausibly-hot-vs-walls are rejected; a cool
// plausible reading passes.
void test_tc_attached_predicate(void) {
  oven_Telemetry t = oven_Telemetry_init_zero;
  t.wall_temp_count = 4;
  for (size_t i = 0; i < 4; ++i) {
    t.wall_temp[i] = 25.0f;
  }
  t.work_temp = 24.0f;
  TEST_ASSERT_TRUE(ConfirmRunScreen::tcAttached(t));
  t.work_temp = std::numeric_limits<float>::quiet_NaN();
  TEST_ASSERT_FALSE(ConfirmRunScreen::tcAttached(t));
  t.work_temp = -50.0f; // open-circuit sentinel
  TEST_ASSERT_FALSE(ConfirmRunScreen::tcAttached(t));
  t.work_temp = 200.0f; // implausibly hotter than the 25 °C walls
  TEST_ASSERT_FALSE(ConfirmRunScreen::tcAttached(t));
}

// §19: Start is gated on the door being CLOSED — in BOTH modes. The hardware interlock enforces
// the door regardless (§4 L0); this is only so the UI never offers an un-runnable Start.
void test_door_open_blocks_start(void) {
  screen.begin(draft(RecipeMode::Cure, "Resin-A"), kSession, cydlink, mgmt);
  screen.render(lv_screen_active());

  feedTelem(24.0f, /*doorOpen=*/true);
  screen.poll();
  TEST_ASSERT_FALSE(screen.ready());

  feedTelem(24.0f, /*doorOpen=*/false); // shut it
  screen.poll();
  TEST_ASSERT_TRUE(screen.ready());
}

// The same gate on the reflow path, where it stacks with the probe precondition.
void test_door_open_blocks_reflow_start_even_with_probe(void) {
  screen.begin(draft(RecipeMode::Reflow, "LF-245"), kSession, cydlink, mgmt);
  screen.render(lv_screen_active());

  feedTelem(24.0f, /*doorOpen=*/true); // probe is fine; the door is not
  screen.poll();
  TEST_ASSERT_FALSE(screen.ready());

  feedTelem(24.0f, /*doorOpen=*/false);
  screen.poll();
  TEST_ASSERT_TRUE(screen.ready());
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_reflow_gate_blocks_without_probe);
  RUN_TEST(test_reflow_commit_starts_run);
  RUN_TEST(test_cure_needs_no_probe);
  RUN_TEST(test_nak_goes_to_failed);
  RUN_TEST(test_link_down_and_cancel);
  RUN_TEST(test_tc_attached_predicate);
  RUN_TEST(test_door_open_blocks_start);
  RUN_TEST(test_door_open_blocks_reflow_start_even_with_probe);
  return UNITY_END();
}
