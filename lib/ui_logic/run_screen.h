// run_screen — the §15 Run / Monitor screen (backlog C7a). The live view of an executing run: it
// arms a RunTracker (lib/app_logic) over the authored draft the Confirm screen committed, feeds it
// the controller's telemetry stream each frame, and renders what the operator watches — the control
// temperature vs setpoint, the current phase ("Soak 2 of 4"), a slipping ETA, a progress bar, the
// UV / fan / turntable indicators — plus the one control that matters mid-run: a full-width,
// single-tap **STOP** (§15/§22 — immediate, not a hold, the opposite of the Confirm arm).
//
// Navigation is LOCKED while running: there is no Back. The only ways off the Running page are
// STOP, or the run reaching a terminal state (DONE / FAULT) in the telemetry — both land on a
// minimal Ended page (outcome word + Done). The rich §16 summary + the §22 fault modal are C8's,
// deferred.
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

  // Fired when the Ended page's Done is tapped (Home is the caller's to rebuild). C8 will replace
  // this hop with the run summary.
  void setExitHandler(void (*cb)(void *user_data), void *user_data);

  // Gesture targets (also directly callable by tests).
  void stop();    // STOP → sendAbort + Ended(Stopped) — immediate, no confirm
  void dismiss(); // Ended Done → exit handler

  Page page() const { return page_; }
  RunOutcome outcome() const { return outcome_; }
  bool stopSent() const { return stop_sent_; }
  const RunTracker &tracker() const { return tracker_; }

private:
  void buildRunning();
  void buildEnded();
  void configParent();
  void clearParent();
  void refresh(const oven_Telemetry &t, bool ours); // update the live fields from a telemetry frame
  void endRun(RunOutcome outcome);

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

  // Live widgets refreshed in place by refresh() (no rebuild per frame). Owned by the widget tree.
  lv_obj_t *temp_lbl_ = nullptr;
  lv_obj_t *setpoint_lbl_ = nullptr;
  lv_obj_t *phase_lbl_ = nullptr; // "Soak 2 of 4 · ~4:32 left" (phase + ETA on one line)
  lv_obj_t *progress_bar_ = nullptr;
  lv_obj_t *uv_pill_ = nullptr;
  lv_obj_t *fan_pill_ = nullptr;
  lv_obj_t *motor_pill_ = nullptr;
  lv_obj_t *door_lbl_ = nullptr;
  char temp_buf_[16]{};
  char set_buf_[24]{};
  char phase_buf_[40]{}; // phase + ETA combined onto one line (fits the short 2.8" landscape)

  void (*on_exit_)(void *) = nullptr;
  void *exit_ud_ = nullptr;

  friend struct RunThunks;
};
