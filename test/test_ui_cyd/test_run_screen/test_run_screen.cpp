// native_ui_cyd suite — the §15 Run / Monitor screen (C7a). Drives RunScreen against a real
// CydLink, injecting telemetry frames directly: the live view follows this run's session, ignores a
// stale frame from a previous run, ends on a terminal state, and STOP fires the abort. Geometry-
// independent; runs at both panel sizes. Pixel layout is reviewed via `make sim-shot`.
#include <cstring>

#include <unity.h>
#include <lvgl.h>
#include "src/debugging/test/lv_test.h"

#include "cyd_link.h"
#include "frame_link.h"
#include "helpers/fake_clock.h"
#include "helpers/pipe_transport.h"
#include "message_router.h"
#include "oven.pb.h"
#include "panel.h"
#include "profile_templates.h"
#include "run_screen.h"
#include "subjects.h"

using Page = RunScreen::Page;

static LoopbackPipe pipe;
static FakeClock clk;
static protocol::MessageRouter cyd_router;
static protocol::FrameLink cyd_link(pipe.a(), TF_MASTER, cyd_router);
static protocol::CydLink cydlink(cyd_link, clk);
static RunScreen screen;

static constexpr uint32_t kSession = 0x5100BEEF;

static bool g_exited;
static void on_exit(void *) {
  g_exited = true;
}

static bool g_again;
static void on_again(void *) {
  g_again = true;
}

static uint32_t g_seq;

void setUp(void) {
  lv_init();
  lv_test_display_create(panel::W, panel::H);
  lv_test_indev_create_all();
  ui_subjects_init();
  cyd_router.setObserver(cydlink);
  cydlink.heartbeat().setEnable(false); // clear leaked authorization from a prior test
  screen.setExitHandler(on_exit, nullptr);
  screen.setRunAgainHandler(on_again, nullptr);
  g_exited = false;
  g_again = false;
  g_seq = 0;
}
void tearDown(void) {
  lv_deinit();
}

static ProfileDraft reflowDraft() {
  ProfileDraft d = profile_templates::defaultTemplate(RecipeMode::Reflow);
  std::strncpy(d.name, "LF-245", kProfileNameCap - 1);
  return d;
}

// Inject a telemetry frame (as if received over the wire).
static void feed(uint32_t session, oven_RunState state, float work, uint32_t seg,
                 uint32_t elapsedMs) {
  oven_Telemetry t = oven_Telemetry_init_zero;
  t.session = session;
  t.seq = ++g_seq;
  t.run_state = state;
  t.work_temp = work;
  t.setpoint = 180.0f;
  t.seg_idx = seg;
  t.elapsed_ms = elapsedMs;
  t.wall_temp_count = 4;
  for (size_t i = 0; i < 4; ++i) {
    t.wall_temp[i] = 60.0f;
  }
  cydlink.onTelemetry(t);
}

// This run's telemetry drives the tracker; the screen stays on the Running page.
void test_running_follows_telemetry(void) {
  screen.begin(reflowDraft(), kSession, cydlink);
  screen.render(lv_screen_active());
  TEST_ASSERT_EQUAL_INT((int)Page::Running, (int)screen.page());
  TEST_ASSERT_EQUAL_UINT32(profile_templates::kReflowPhases, screen.tracker().phaseCount());

  feed(kSession, oven_RunState_RUN_STATE_RUNNING, 150.0f, /*seg=*/1, /*elapsed=*/12000);
  screen.poll();
  TEST_ASSERT_EQUAL_INT((int)Page::Running, (int)screen.page());
  TEST_ASSERT_EQUAL_UINT32(1, screen.tracker().segIdx());
  TEST_ASSERT_EQUAL_UINT32(12000, screen.tracker().elapsedMs());
}

// A frame from a different session (a previous run's tail) is ignored — it neither drives the
// tracker nor ends this run.
void test_stale_session_ignored(void) {
  screen.begin(reflowDraft(), kSession, cydlink);
  screen.render(lv_screen_active());
  feed(0x9999AAAA, oven_RunState_RUN_STATE_DONE, 40.0f, /*seg=*/4, /*elapsed=*/9999);
  screen.poll();
  TEST_ASSERT_EQUAL_INT((int)Page::Running, (int)screen.page()); // not ended
  TEST_ASSERT_EQUAL_UINT32(0, screen.tracker().elapsedMs());     // tracker untouched
}

// DONE after the run has actually been RUNNING ends it Completed.
void test_done_ends_completed(void) {
  screen.begin(reflowDraft(), kSession, cydlink);
  screen.render(lv_screen_active());
  feed(kSession, oven_RunState_RUN_STATE_RUNNING, 200.0f, 2, 30000);
  screen.poll();
  feed(kSession, oven_RunState_RUN_STATE_DONE, 43.0f, 4, 600000);
  screen.poll();
  TEST_ASSERT_EQUAL_INT((int)Page::Ended, (int)screen.page());
  TEST_ASSERT_EQUAL_INT((int)RunOutcome::Completed, (int)screen.outcome());
}

// A stale DONE before this run has been seen RUNNING must NOT end it (guard against a prior run's
// terminal frame carrying our session by fluke).
void test_done_before_running_holds(void) {
  screen.begin(reflowDraft(), kSession, cydlink);
  screen.render(lv_screen_active());
  feed(kSession, oven_RunState_RUN_STATE_DONE, 43.0f, 4, 0);
  screen.poll();
  TEST_ASSERT_EQUAL_INT((int)Page::Running, (int)screen.page());
}

// A fault ends the run Fault.
void test_fault_ends_fault(void) {
  screen.begin(reflowDraft(), kSession, cydlink);
  screen.render(lv_screen_active());
  feed(kSession, oven_RunState_RUN_STATE_RUNNING, 210.0f, 2, 40000);
  screen.poll();
  feed(kSession, oven_RunState_RUN_STATE_FAULT, 250.0f, 2, 41000);
  screen.poll();
  TEST_ASSERT_EQUAL_INT((int)Page::Ended, (int)screen.page());
  TEST_ASSERT_EQUAL_INT((int)RunOutcome::Fault, (int)screen.outcome());
}

// STOP fires the abort (de-authorizes locally) and ends the run Stopped, immediately — no telemetry
// required.
void test_stop_aborts_and_ends(void) {
  screen.begin(reflowDraft(), kSession, cydlink);
  screen.render(lv_screen_active());
  cydlink.heartbeat().setEnable(true); // a run is authorized...
  feed(kSession, oven_RunState_RUN_STATE_RUNNING, 180.0f, 2, 20000);
  screen.poll();

  screen.stop();
  TEST_ASSERT_TRUE(screen.stopSent());
  TEST_ASSERT_FALSE(cydlink.heartbeat().enable()); // ...and STOP cut it
  TEST_ASSERT_EQUAL_INT((int)Page::Ended, (int)screen.page());
  TEST_ASSERT_EQUAL_INT((int)RunOutcome::Stopped, (int)screen.outcome());
}

// The summary's Home exits to the caller.
void test_dismiss_exits(void) {
  screen.begin(reflowDraft(), kSession, cydlink);
  screen.render(lv_screen_active());
  screen.stop(); // → Ended
  screen.dismiss();
  TEST_ASSERT_TRUE(g_exited);
}

// --- §16 Run Summary (C8) ---

// A completed run computes the fit the summary renders, over the phases actually walked.
void test_summary_completed_computes_fit(void) {
  screen.begin(reflowDraft(), kSession, cydlink);
  screen.render(lv_screen_active());
  for (uint32_t i = 0; i < 4; ++i) {
    feed(kSession, oven_RunState_RUN_STATE_RUNNING, 150.0f + static_cast<float>(i) * 20.0f, i,
         10000 * (i + 1));
    screen.poll();
  }
  feed(kSession, oven_RunState_RUN_STATE_DONE, 43.0f, 4, 600000);
  screen.poll();

  TEST_ASSERT_EQUAL_INT((int)Page::Ended, (int)screen.page());
  TEST_ASSERT_TRUE(screen.fit().computed);
  // The fit counts the phases actually ENTERED, not the recipe's total — a phase compiles to a
  // ramp + a hold, so walking 4 segments enters fewer than 4 phases. The summary prints
  // "<reached>/<phaseCount> on target" off this, so it must never exceed the authored count.
  TEST_ASSERT_GREATER_THAN_UINT32(0, screen.fit().phaseCount);
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(profile_templates::kReflowPhases, screen.fit().phaseCount);
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(screen.fit().phaseCount, screen.fit().phasesMissedTarget);
  // The numbers the summary prints must be finite — they reach an lv_label either way.
  TEST_ASSERT_TRUE(screen.fit().runQuality.maxAbsC == screen.fit().runQuality.maxAbsC);
  TEST_ASSERT_TRUE(screen.fit().runQuality.rmsC == screen.fit().runQuality.rmsC);
}

// §16: "abort/fault skip it — data incomplete". A stopped run has no verdict and no advisory, so
// the summary shows the no-fit line instead of numbers derived from a partial run.
void test_summary_stopped_skips_fit(void) {
  screen.begin(reflowDraft(), kSession, cydlink);
  screen.render(lv_screen_active());
  feed(kSession, oven_RunState_RUN_STATE_RUNNING, 180.0f, 2, 20000);
  screen.poll();
  screen.stop();

  TEST_ASSERT_EQUAL_INT((int)RunOutcome::Stopped, (int)screen.outcome());
  TEST_ASSERT_FALSE(screen.fit().computed);
  TEST_ASSERT_FALSE(screen.fit().advisory);
}

// A fault likewise skips the fit, but the summary keeps the CAUSE — §16's "(+ cause)". The §22
// modal is dismissable; this page is the run's record and outlives it.
void test_summary_fault_keeps_cause(void) {
  screen.begin(reflowDraft(), kSession, cydlink);
  screen.render(lv_screen_active());
  feed(kSession, oven_RunState_RUN_STATE_RUNNING, 210.0f, 2, 40000);
  screen.poll();

  oven_Telemetry t = oven_Telemetry_init_zero;
  t.session = kSession;
  t.seq = ++g_seq;
  t.run_state = oven_RunState_RUN_STATE_FAULT;
  t.fault_code = oven_FaultCode_FAULT_OVERTEMP_CHAMBER;
  t.work_temp = 268.0f;
  cydlink.onTelemetry(t);
  screen.poll();

  TEST_ASSERT_EQUAL_INT((int)RunOutcome::Fault, (int)screen.outcome());
  TEST_ASSERT_FALSE(screen.fit().computed);
  TEST_ASSERT_EQUAL_INT((int)oven_FaultCode_FAULT_OVERTEMP_CHAMBER, (int)screen.faultCode());
}

// "Run again" is its own seam, separate from Home: §19 forbids a one-tap re-energize, so the
// composition root routes it back through Confirm rather than restarting here.
void test_summary_run_again_routes(void) {
  screen.begin(reflowDraft(), kSession, cydlink);
  screen.render(lv_screen_active());
  screen.stop(); // → Ended
  screen.runAgain();
  TEST_ASSERT_TRUE(g_again);
  TEST_ASSERT_FALSE(g_exited); // Run again is NOT the Home hop
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_running_follows_telemetry);
  RUN_TEST(test_stale_session_ignored);
  RUN_TEST(test_done_ends_completed);
  RUN_TEST(test_done_before_running_holds);
  RUN_TEST(test_fault_ends_fault);
  RUN_TEST(test_stop_aborts_and_ends);
  RUN_TEST(test_dismiss_exits);
  RUN_TEST(test_summary_completed_computes_fit);
  RUN_TEST(test_summary_stopped_skips_fit);
  RUN_TEST(test_summary_fault_keeps_cause);
  RUN_TEST(test_summary_run_again_routes);
  return UNITY_END();
}
