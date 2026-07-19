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

static bool g_resumed;
static ProfileDraft g_resume_draft;
static void on_resume(void *, const ProfileDraft &d) {
  g_resumed = true;
  g_resume_draft = d;
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
  screen.setResumeHandler(on_resume, nullptr);
  g_exited = false;
  g_again = false;
  g_resumed = false;
  g_resume_draft = ProfileDraft{};
  g_seq = 0;
}
void tearDown(void) {
  lv_deinit();
}

static ProfileDraft cureDraft() {
  ProfileDraft d = profile_templates::defaultTemplate(RecipeMode::Cure);
  std::strncpy(d.name, "Resin-A", kProfileNameCap - 1);
  return d;
}

static ProfileDraft reflowDraft() {
  ProfileDraft d = profile_templates::defaultTemplate(RecipeMode::Reflow);
  std::strncpy(d.name, "LF-245", kProfileNameCap - 1);
  return d;
}

// Inject a telemetry frame (as if received over the wire).
static void feed(uint32_t session, oven_RunState state, float work, uint32_t seg,
                 uint32_t elapsedMs, bool doorOpen = false) {
  oven_Telemetry t = oven_Telemetry_init_zero;
  t.session = session;
  t.seq = ++g_seq;
  t.run_state = state;
  t.work_temp = work;
  t.setpoint = 180.0f;
  t.seg_idx = seg;
  t.elapsed_ms = elapsedMs;
  t.door_open = doorOpen;
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

// --- §15 door-open (PR3) ---

// The controller safes and ends the run to IDLE on a door-open — no fault, no DONE. The CYD reads
// that as its own outcome so the summary can say what actually happened.
void test_door_open_ends_run_aborted(void) {
  screen.begin(reflowDraft(), kSession, cydlink);
  screen.render(lv_screen_active());
  cydlink.heartbeat().setEnable(true);
  feed(kSession, oven_RunState_RUN_STATE_RUNNING, 180.0f, 2, 20000);
  screen.poll();

  feed(kSession, oven_RunState_RUN_STATE_IDLE, 150.0f, 2, 21000, /*doorOpen=*/true);
  screen.poll();

  TEST_ASSERT_EQUAL_INT((int)Page::Ended, (int)screen.page());
  TEST_ASSERT_EQUAL_INT((int)RunOutcome::DoorOpened, (int)screen.outcome());
  TEST_ASSERT_FALSE(screen.fit().computed);        // an incomplete run has no fit (§16)
  TEST_ASSERT_FALSE(cydlink.heartbeat().enable()); // de-authorized: the run is over
}

// The door that ENDED the run must not immediately dismiss the summary — the operator has not seen
// it yet. Only a fresh open (they came back to take the board out) dismisses.
void test_door_that_ended_run_does_not_dismiss_summary(void) {
  screen.begin(reflowDraft(), kSession, cydlink);
  screen.render(lv_screen_active());
  feed(kSession, oven_RunState_RUN_STATE_RUNNING, 180.0f, 2, 20000);
  screen.poll();
  feed(kSession, oven_RunState_RUN_STATE_IDLE, 150.0f, 2, 21000, /*doorOpen=*/true);
  screen.poll();
  TEST_ASSERT_EQUAL_INT((int)RunOutcome::DoorOpened, (int)screen.outcome());

  for (int i = 0; i < 5; ++i) { // door still open, frames keep arriving
    feed(kSession, oven_RunState_RUN_STATE_IDLE, 140.0f, 2, 22000, /*doorOpen=*/true);
    screen.poll();
  }
  TEST_ASSERT_FALSE(g_exited); // summary still up
}

// The bench ask: on a completed run, opening the door IS the acknowledgement — clear to Home.
void test_door_open_dismisses_completed_summary(void) {
  screen.begin(reflowDraft(), kSession, cydlink);
  screen.render(lv_screen_active());
  feed(kSession, oven_RunState_RUN_STATE_RUNNING, 200.0f, 2, 30000);
  screen.poll();
  feed(kSession, oven_RunState_RUN_STATE_DONE, 43.0f, 4, 600000);
  screen.poll();
  TEST_ASSERT_EQUAL_INT((int)RunOutcome::Completed, (int)screen.outcome());
  TEST_ASSERT_FALSE(g_exited);

  feed(kSession, oven_RunState_RUN_STATE_DONE, 43.0f, 4, 601000, /*doorOpen=*/true);
  screen.poll();
  TEST_ASSERT_TRUE(g_exited);
}

// ...but a FAULT summary is NOT door-dismissable (§22): a fault demands an explicit acknowledge,
// and opening the door to look at a scorched board must not clear the record of why it scorched.
void test_door_open_does_not_dismiss_fault_summary(void) {
  screen.begin(reflowDraft(), kSession, cydlink);
  screen.render(lv_screen_active());
  feed(kSession, oven_RunState_RUN_STATE_RUNNING, 210.0f, 2, 40000);
  screen.poll();
  feed(kSession, oven_RunState_RUN_STATE_FAULT, 250.0f, 2, 41000);
  screen.poll();
  TEST_ASSERT_EQUAL_INT((int)RunOutcome::Fault, (int)screen.outcome());

  for (int i = 0; i < 5; ++i) {
    feed(kSession, oven_RunState_RUN_STATE_FAULT, 250.0f, 2, 42000, /*doorOpen=*/true);
    screen.poll();
  }
  TEST_ASSERT_FALSE(g_exited); // still demanding the explicit tap
}

// --- §15 cure Paused / Resume (C7 PR3 over B6) ---

// A cure interrupted mid-run pauses instead of aborting: the operator can shut the door and finish
// it. Reflow, by contrast, aborts outright (covered above) — §15's explicit mode split.
void test_cure_door_open_pauses(void) {
  screen.begin(cureDraft(), kSession, cydlink);
  screen.render(lv_screen_active());
  cydlink.heartbeat().setEnable(true);
  feed(kSession, oven_RunState_RUN_STATE_RUNNING, 55.0f, 1, 30000);
  screen.poll();

  feed(kSession, oven_RunState_RUN_STATE_IDLE, 50.0f, 1, 31000, /*doorOpen=*/true);
  screen.poll();

  TEST_ASSERT_EQUAL_INT((int)Page::Paused, (int)screen.page());
  TEST_ASSERT_GREATER_THAN_UINT32(0, screen.remainder().phaseCount);
  TEST_ASSERT_EQUAL_INT((int)RecipeMode::Cure, (int)screen.remainder().mode);
}

// §15: Resume is "enabled only when the door is closed" — no auto-resume, eye safety. The gate is
// live, so shutting the door on the Paused page enables it without leaving the page.
void test_resume_gated_on_door_closed(void) {
  screen.begin(cureDraft(), kSession, cydlink);
  screen.render(lv_screen_active());
  feed(kSession, oven_RunState_RUN_STATE_RUNNING, 55.0f, 1, 30000);
  screen.poll();
  feed(kSession, oven_RunState_RUN_STATE_IDLE, 50.0f, 1, 31000, /*doorOpen=*/true);
  screen.poll();
  TEST_ASSERT_EQUAL_INT((int)Page::Paused, (int)screen.page());
  TEST_ASSERT_FALSE(screen.canResume());

  screen.resume(); // guarded: the gesture must do nothing while the door is open
  TEST_ASSERT_FALSE(g_resumed);

  feed(kSession, oven_RunState_RUN_STATE_IDLE, 48.0f, 1, 32000, /*doorOpen=*/false);
  screen.poll();
  TEST_ASSERT_TRUE(screen.canResume());
}

// Resuming hands the composition root the REMAINDER, not the original — that is the whole point:
// the controller sees an ordinary new profile and needs no pause state of its own.
void test_resume_starts_the_remainder(void) {
  screen.begin(cureDraft(), kSession, cydlink);
  screen.render(lv_screen_active());
  feed(kSession, oven_RunState_RUN_STATE_RUNNING, 55.0f, 1, 30000);
  screen.poll();
  feed(kSession, oven_RunState_RUN_STATE_IDLE, 50.0f, 1, 31000, /*doorOpen=*/true);
  screen.poll();
  feed(kSession, oven_RunState_RUN_STATE_IDLE, 48.0f, 1, 32000, /*doorOpen=*/false);
  screen.poll();

  screen.resume();
  TEST_ASSERT_TRUE(g_resumed);
  TEST_ASSERT_GREATER_THAN_UINT32(0, g_resume_draft.phaseCount);
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(cureDraft().phaseCount, g_resume_draft.phaseCount);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, g_resume_draft.phases[0].rampSeconds); // §15's ASAP re-heat
}

// Abort from the Paused page gives up on the cure and lands on the summary.
void test_paused_abort_ends_run(void) {
  screen.begin(cureDraft(), kSession, cydlink);
  screen.render(lv_screen_active());
  feed(kSession, oven_RunState_RUN_STATE_RUNNING, 55.0f, 1, 30000);
  screen.poll();
  feed(kSession, oven_RunState_RUN_STATE_IDLE, 50.0f, 1, 31000, /*doorOpen=*/true);
  screen.poll();

  screen.abandon();
  TEST_ASSERT_EQUAL_INT((int)Page::Ended, (int)screen.page());
  TEST_ASSERT_EQUAL_INT((int)RunOutcome::DoorOpened, (int)screen.outcome());
}

// §15: "a lost heartbeat also ends it (the controller is already idle)". A pause we cannot confirm
// the machine's state through is not a pause worth keeping.
void test_paused_ends_on_lost_link(void) {
  screen.begin(cureDraft(), kSession, cydlink);
  screen.render(lv_screen_active());
  feed(kSession, oven_RunState_RUN_STATE_RUNNING, 55.0f, 1, 30000);
  screen.poll();
  feed(kSession, oven_RunState_RUN_STATE_IDLE, 50.0f, 1, 31000, /*doorOpen=*/true);
  screen.poll();
  TEST_ASSERT_EQUAL_INT((int)Page::Paused, (int)screen.page());

  clk.advance(protocol::kLinkTimeoutMs + 1); // telemetry stops arriving
  screen.poll();
  TEST_ASSERT_EQUAL_INT((int)Page::Ended, (int)screen.page());
}

// The resumed run's CHART spans the ORIGINAL job, not just the remainder: the operator interrupted
// one cure and wants to see the whole of it. The tracker still tracks the remainder (that is what
// is executing), so the two are deliberately different things.
void test_resumed_chart_keeps_the_original_timeline(void) {
  screen.begin(cureDraft(), kSession, cydlink);
  screen.render(lv_screen_active());
  for (int i = 1; i <= 6; ++i) {
    feed(kSession, oven_RunState_RUN_STATE_RUNNING, 40.0f + static_cast<float>(i), 1,
         static_cast<uint32_t>(i) * 20000);
    screen.poll();
  }
  feed(kSession, oven_RunState_RUN_STATE_IDLE, 50.0f, 1, 140000, /*doorOpen=*/true);
  screen.poll();
  TEST_ASSERT_EQUAL_INT((int)Page::Paused, (int)screen.page());
  const float pausedAt = screen.tracker().progress01();
  TEST_ASSERT_TRUE(pausedAt > 0.0f); // the run got somewhere before the door opened

  const ProfileDraft rem = screen.remainder();
  screen.beginResumed(rem, kSession + 1, cydlink);
  screen.render(lv_screen_active());

  TEST_ASSERT_TRUE(screen.resumed());
  // The resumed leg starts JUST AFTER the pause point in the ORIGINAL timeline — strictly after, so
  // it cannot overwrite the trace it is continuing, and close behind, so the two read as one job.
  // The exact offset is quantised to chart columns (two of them: one blank column for the gap).
  TEST_ASSERT_TRUE(screen.resumeFrom01() > pausedAt);
  const float kColumn = 1.0f / static_cast<float>(RunScreen::kCurvePoints - 1);
  TEST_ASSERT_TRUE(screen.resumeFrom01() - pausedAt < 4.0f * kColumn);
  // ...and the tracker is armed on the REMAINDER, which is a shorter job than the original.
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(cureDraft().phaseCount, screen.tracker().phaseCount());
  TEST_ASSERT_EQUAL_UINT32(rem.phaseCount, screen.tracker().phaseCount());
}

// A resumed run that is 0% through its remainder plots at the PAUSE point, not at the chart's left
// edge — otherwise the resumed leg would overwrite the part of the trace it is meant to continue.
void test_resumed_samples_land_after_the_pause_point(void) {
  screen.begin(cureDraft(), kSession, cydlink);
  screen.render(lv_screen_active());
  for (int i = 1; i <= 6; ++i) {
    feed(kSession, oven_RunState_RUN_STATE_RUNNING, 40.0f + static_cast<float>(i), 1,
         static_cast<uint32_t>(i) * 20000);
    screen.poll();
  }
  feed(kSession, oven_RunState_RUN_STATE_IDLE, 50.0f, 1, 140000, /*doorOpen=*/true);
  screen.poll();

  screen.beginResumed(screen.remainder(), kSession + 1, cydlink);
  screen.render(lv_screen_active());
  TEST_ASSERT_TRUE(screen.resumeFrom01() > 0.0f);

  // A fresh run maps identically; a resumed one is offset by the pause point.
  screen.begin(cureDraft(), kSession + 2, cydlink);
  TEST_ASSERT_FALSE(screen.resumed());
  TEST_ASSERT_EQUAL_FLOAT(0.0f, screen.resumeFrom01());
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
  RUN_TEST(test_door_open_ends_run_aborted);
  RUN_TEST(test_door_that_ended_run_does_not_dismiss_summary);
  RUN_TEST(test_door_open_dismisses_completed_summary);
  RUN_TEST(test_door_open_does_not_dismiss_fault_summary);
  RUN_TEST(test_cure_door_open_pauses);
  RUN_TEST(test_resume_gated_on_door_closed);
  RUN_TEST(test_resume_starts_the_remainder);
  RUN_TEST(test_paused_abort_ends_run);
  RUN_TEST(test_paused_ends_on_lost_link);
  RUN_TEST(test_resumed_chart_keeps_the_original_timeline);
  RUN_TEST(test_resumed_samples_land_after_the_pause_point);
  return UNITY_END();
}
