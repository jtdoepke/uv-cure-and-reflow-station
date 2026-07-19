// run_screen — the §15 Run / Monitor screen (backlog C7a). The live view of an executing run: it
// arms a RunTracker (lib/app_logic) over the authored draft the Confirm screen committed, feeds it
// the controller's telemetry stream each frame, and renders what the operator watches — the control
// temperature vs setpoint, the current phase ("Soak 2 of 4"), a slipping ETA, a progress bar, the
// UV / fan / turntable indicators — plus the one control that matters mid-run: a full-width,
// single-tap **STOP** (§15/§22 — immediate, not a hold, the opposite of the Confirm arm).
//
// Navigation is LOCKED while running: there is no Back. The only ways off the Running page are
// STOP, or the run reaching a terminal state (DONE / FAULT) in the telemetry — both land on the
// **Ended page, which is the §16 Run Summary** (C8): outcome badge, the completed
// projected-vs-actual overlay, the fit verdict + key numbers, the amber drift advisory, and
// Run again / Home. The §22 fault modal is separate (fault_overlay, lv_layer_top) — it draws OVER
// this screen and its Acknowledge lands here, on the Fault-outcome summary.
//
// The summary is the Ended *page* rather than its own router screen on purpose: it renders this
// screen's own RunTracker (the fit result is only computed here, at endRun) and re-draws this
// screen's own curve, so a separate screen would exist only to be handed both across a boundary.
// It also gives §22's AckRoute::RunSummary a target that is already on screen.
//
// Telemetry is filtered by session (§9): only frames carrying THIS run's session drive the view, so
// a stale frame from a previous run can't spuriously end this one. The cross-screen subj_run_state
// (sleep suppression, §17) is driven by the firmware's telemetry gateway, not here.
//
// begin()/render()/poll() split like the editor/confirm. References lib/protocol (CydLink) +
// lib/app_logic (RunTracker) — host-testable; LVGL-only view. Compiles for firmware +
// native_ui_cyd.
#pragma once

#include <cstdint>

#include <lvgl.h>

#include "cyd_link.h"
#include "oven_cal.h"
#include "profile_draft.h"
#include "fault_table.h"
#include "run_curve.h"
#include "run_tracker.h"

class RunScreen {
public:
  enum class Page { Running, Ended };

  // Arm the run over `draft` (the committed authored profile) with the run's `session` (§9: only
  // telemetry carrying it is this run's). `link` supplies telemetry + the STOP abort. All
  // references must outlive this screen.
  void begin(const ProfileDraft &draft, uint32_t session, protocol::CydLink &link,
             const OvenModel &model = oven_cal::kDefaultModel);

  // Build the current page under `parent` (the router build cb; call after begin()).
  void render(lv_obj_t *parent);

  // Drive the view: call every loop after link.service(). Feeds new telemetry frames to the
  // tracker, refreshes the live fields, and transitions to Ended on a terminal telemetry state.
  void poll();

  // Fired when the summary's Home is tapped (Home is the caller's to rebuild).
  void setExitHandler(void (*cb)(void *user_data), void *user_data);
  // Fired when the summary's "Run again" is tapped. §19 forbids a one-tap re-energize, so the
  // caller routes this back through Confirm — never straight to heat.
  void setRunAgainHandler(void (*cb)(void *user_data), void *user_data);

  // Gesture targets (also directly callable by tests).
  void stop();     // STOP → sendAbort + Ended(Stopped) — immediate, no confirm
  void dismiss();  // summary Home → exit handler
  void runAgain(); // summary Run again → run-again handler (→ Confirm)

  Page page() const { return page_; }
  RunOutcome outcome() const { return outcome_; }
  bool stopSent() const { return stop_sent_; }
  const RunTracker &tracker() const { return tracker_; }
  // The §16 fit, computed once at endRun(). `computed` is false for a non-Completed outcome —
  // "abort/fault skip it — data incomplete" — which is what gates the verdict + advisory below.
  const RunFitResult &fit() const { return fit_; }
  // The cause behind a Fault outcome (FAULT_NONE otherwise), as rendered in the badge. Raw wire
  // int — see fault_table.h on why an untrusted code is never held as the enum type.
  fault_table::FaultCodeWire faultCode() const { return fault_code_; }
  // The authored draft this run executed — what "Run again" re-confirms.
  const ProfileDraft &draft() const { return draft_; }

  // Chart resolution across the run — the retained sample buffers are sized to it.
  static constexpr uint16_t kCurvePoints = 48;

private:
  void buildRunning();
  void buildEnded();
  void configParent();
  void clearParent();
  void refresh(const oven_Telemetry &t, bool ours); // update the live fields from a telemetry frame
  void pollEnded(); // summary page: the §15 door-open dismiss (never on a Fault outcome)
  void endRun(RunOutcome outcome);

  // Sample the projected trajectory into proj_ and derive the chart's y-range. Once per run.
  void prepareCurve();
  // Record a measured point into actual_ with the same index/gap-fill rule the widget uses, so the
  // retained buffer and the live trace agree. The buffer OUTLIVES the widget tree: the summary
  // re-draws the completed run after buildEnded() has freed the Running page's chart.
  void recordActual(float frac01, float valueC);
  // Build the chart under `parent_`, replaying every retained actual sample. Used by both pages —
  // live (grown per frame after this) and complete (the §16 overlay).
  void buildCurve();

  // The mode's control variable (§5): reflow tracks the workpiece TC, cure the hottest wall
  // channel.
  float controlTempC(const oven_Telemetry &t) const;

  lv_obj_t *parent_ = nullptr;
  Page page_ = Page::Running;

  ProfileDraft draft_{};
  uint32_t session_ = 0;
  protocol::CydLink *link_ = nullptr;
  const OvenModel *model_ = &oven_cal::kDefaultModel;
  RunTracker tracker_;
  uint32_t last_seq_ = 0;
  bool have_seq_ = false;
  bool saw_running_ = false; // guards against a stale DONE before this run actually ran
  RunOutcome outcome_ = RunOutcome::Completed;
  bool stop_sent_ = false;
  RunFitResult fit_{}; // §16, computed at endRun()
  // The cause behind a Fault outcome, for §16's "outcome badge ... (+ cause)". Captured from the
  // terminal frame: the §22 overlay knows it too, but it is dismissable and the summary outlives
  // it.
  fault_table::FaultCodeWire fault_code_ = oven_FaultCode_FAULT_NONE;

  // Retained curve data — survives the page swap so the summary can draw the COMPLETED overlay
  // (§16 "both curves complete"). The live widget's own buffers die with its widget tree.
  float proj_[kCurvePoints]{};
  float actual_[kCurvePoints]{};
  int32_t actual_last_ = -1; // highest filled index in actual_ (-1 = no telemetry yet)
  int32_t y_lo_ = 0;
  int32_t y_hi_ = 0;
  bool deviated_ = false;      // last-applied cue colour, replayed onto a rebuilt chart
  bool door_was_open_ = false; // edge detector for the summary's door dismiss

  // Live widgets refreshed in place by refresh() (no rebuild per frame). Owned by the widget tree.
  lv_obj_t *temp_lbl_ = nullptr;
  lv_obj_t *setpoint_lbl_ = nullptr;
  lv_obj_t *phase_lbl_ = nullptr; // "Soak 2 of 4 · ~4:32 left" (phase + ETA on one line)
  lv_obj_t *cue_lbl_ = nullptr;   // door / deviation / on-plan status line (§15/§16)
  lv_obj_t *uv_pill_ = nullptr;
  lv_obj_t *fan_pill_ = nullptr;
  lv_obj_t *motor_pill_ = nullptr;
  RunCurve run_curve_{}; // projected-vs-actual chart (C7b)
  char temp_buf_[16]{};
  char set_buf_[24]{};
  char phase_buf_[56]{}; // target · phase · ETA on one line (fits the short 2.8" landscape)
  char stats_buf_[72]{}; // the summary's verdict · max · rms · phases line

  void (*on_exit_)(void *) = nullptr;
  void *exit_ud_ = nullptr;
  void (*on_again_)(void *) = nullptr;
  void *again_ud_ = nullptr;

  friend struct RunThunks;
};
