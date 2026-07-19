#include "run_screen.h"

#include <cstdio>

#include "link_banner.h"
#include "profile_facts.h" // formatDuration / formatPeak
#include "recipe_compiler.h"
#include "subjects.h"
#include "theme.h"

struct RunThunks {
  static void stop_evt(lv_event_t *e) {
    static_cast<RunScreen *>(lv_event_get_user_data(e))->stop();
  }
  static void done_evt(lv_event_t *e) {
    static_cast<RunScreen *>(lv_event_get_user_data(e))->dismiss();
  }
};

// --- Lifecycle ---

void RunScreen::begin(const ProfileDraft &draft, uint32_t session, protocol::CydLink &link,
                      const OvenModel &model) {
  draft_ = draft;
  session_ = session;
  link_ = &link;
  model_ = &model;
  last_seq_ = 0;
  have_seq_ = false;
  saw_running_ = false;
  stop_sent_ = false;
  outcome_ = RunOutcome::Completed;
  page_ = Page::Running;

  // Arm the tracker over the same compile inputs the recipe was uploaded with (§15): the mode's
  // user cap (§24) floored at 0, the shared default ambient — so its segment→phase map matches the
  // recipe the controller is executing.
  const int32_t cap = draft_.mode == RecipeMode::Cure ? lv_subject_get_int(&subj_uv_cap)
                                                      : lv_subject_get_int(&subj_reflow_cap);
  const Caps caps{0.0f, static_cast<float>(cap)};
  tracker_.begin(draft_, *model_, caps, profile_facts::kDefaultAmbientC, lv_tick_get());
}

void RunScreen::render(lv_obj_t *parent) {
  parent_ = parent;
  if (page_ == Page::Running) {
    buildRunning();
  } else {
    buildEnded();
  }
}

void RunScreen::setExitHandler(void (*cb)(void *), void *user_data) {
  on_exit_ = cb;
  exit_ud_ = user_data;
}

float RunScreen::controlTempC(const oven_Telemetry &t) const {
  if (draft_.mode == RecipeMode::Cure) {
    float hottest = -1.0e30f;
    const size_t n = t.wall_temp_count <= 4 ? t.wall_temp_count : 4;
    for (size_t i = 0; i < n; ++i) {
      if (t.wall_temp[i] > hottest) {
        hottest = t.wall_temp[i];
      }
    }
    return n > 0 ? hottest : t.work_temp;
  }
  return t.work_temp;
}

// --- poll(): telemetry → tracker + live refresh + terminal transition ---

void RunScreen::poll() {
  if (link_ == nullptr || page_ != Page::Running) {
    return;
  }
  const bool alive = link_->linkAlive() && link_->hasTelemetry();
  if (!alive) {
    return; // the link banner (subject-bound) already shows the drop; hold the last view
  }
  const oven_Telemetry &t = link_->lastTelemetry();
  const bool ours = (t.session == session_); // §9: ignore a stale frame from a previous run

  if (ours && (!have_seq_ || t.seq != last_seq_)) {
    last_seq_ = t.seq;
    have_seq_ = true;
    tracker_.update(t, lv_tick_get());
  }
  refresh(t, ours);

  if (!ours) {
    return; // our run's frames haven't started arriving yet — nothing terminal to read
  }
  if (t.run_state == oven_RunState_RUN_STATE_RUNNING) {
    saw_running_ = true;
  }
  if (t.fault_code != oven_FaultCode_FAULT_NONE || t.run_state == oven_RunState_RUN_STATE_FAULT) {
    endRun(RunOutcome::Fault);
  } else if (t.run_state == oven_RunState_RUN_STATE_DONE && saw_running_) {
    // Guard on saw_running_: the very first frames can still carry a prior run's DONE until the
    // controller adopts our session's RUNNING.
    endRun(RunOutcome::Completed);
  }
}

void RunScreen::refresh(const oven_Telemetry &t, bool ours) {
  if (temp_lbl_ == nullptr) {
    return; // not on the Running page
  }
  if (!ours) {
    lv_label_set_text(temp_lbl_, "--\xC2\xB0");
    if (setpoint_lbl_ != nullptr) {
      lv_label_set_text(setpoint_lbl_, "starting...");
    }
    return;
  }
  std::snprintf(temp_buf_, sizeof(temp_buf_), "%.0f\xC2\xB0", static_cast<double>(controlTempC(t)));
  lv_label_set_text(temp_lbl_, temp_buf_);
  if (setpoint_lbl_ != nullptr) {
    std::snprintf(set_buf_, sizeof(set_buf_), "target %.0f\xC2\xB0",
                  static_cast<double>(t.setpoint));
    lv_label_set_text(setpoint_lbl_, set_buf_);
  }
  if (phase_lbl_ != nullptr) {
    char dur[16];
    profile_facts::formatDuration(tracker_.etaSeconds(), dur, sizeof(dur));
    if (tracker_.inCooldown()) {
      std::snprintf(phase_buf_, sizeof(phase_buf_), "Cooling \xC2\xB7 %s left", dur);
    } else {
      std::snprintf(phase_buf_, sizeof(phase_buf_), "%s %u of %u \xC2\xB7 %s left",
                    tracker_.phaseName(), static_cast<unsigned>(tracker_.phaseOrdinal()),
                    static_cast<unsigned>(tracker_.phaseCount()), dur);
    }
    lv_label_set_text(phase_lbl_, phase_buf_);
  }
  if (progress_bar_ != nullptr) {
    lv_bar_set_value(progress_bar_, static_cast<int32_t>(tracker_.progress01() * 100.0f),
                     LV_ANIM_OFF);
  }
  // Indicators: lit in their reserved hue when the output is on, receded when off (word + colour,
  // so they read without relying on colour alone — the label text is always present).
  if (uv_pill_ != nullptr) {
    lv_obj_set_style_text_color(uv_pill_,
                                theme::col(t.uv_duty > 0.0f ? theme::UV : theme::TEXT_DIM), 0);
  }
  if (fan_pill_ != nullptr) {
    lv_obj_set_style_text_color(fan_pill_, theme::col(t.conv_fan ? theme::ACCENT : theme::TEXT_DIM),
                                0);
  }
  if (motor_pill_ != nullptr) {
    lv_obj_set_style_text_color(motor_pill_, theme::col(t.motor ? theme::ACCENT : theme::TEXT_DIM),
                                0);
  }
  if (door_lbl_ != nullptr) {
    lv_label_set_text(door_lbl_, t.door_open ? LV_SYMBOL_WARNING " DOOR OPEN" : "");
    lv_obj_set_style_text_color(door_lbl_, theme::col(theme::WARN), 0);
  }
}

void RunScreen::endRun(RunOutcome outcome) {
  outcome_ = outcome;
  tracker_.finish(outcome, lv_tick_get()); // computes the §16 fit result (C8 will render it)
  page_ = Page::Ended;
  buildEnded();
}

// --- Gestures ---

void RunScreen::stop() {
  if (link_ != nullptr) {
    link_->sendAbort(); // §15: immediate, fire-and-forget — de-authorizes locally + sends Abort
  }
  stop_sent_ = true;
  endRun(RunOutcome::Stopped);
}

void RunScreen::dismiss() {
  if (on_exit_ != nullptr) {
    on_exit_(exit_ud_);
  }
}

// --- Shared building blocks ---

void RunScreen::clearParent() {
  lv_obj_clean(parent_);
}

void RunScreen::configParent() {
  theme::apply_screen(parent_);
  lv_obj_set_flex_flow(parent_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(parent_, theme::PAD_S, 0);
  lv_obj_set_style_pad_row(parent_, theme::GAP, 0);
}

// --- Running page: the live monitor. No Back — the nav lock (§15) ---

namespace {
lv_obj_t *make_indicator(lv_obj_t *row, const char *text) {
  lv_obj_t *lbl = lv_label_create(row);
  lv_label_set_text(lbl, text);
  lv_obj_set_flex_grow(lbl, 1);
  lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(lbl, theme::col(theme::TEXT_DIM), 0);
  return lbl;
}
} // namespace

void RunScreen::buildRunning() {
  clearParent();
  configParent();

  // Header: profile name + a RUNNING badge. Deliberately no Back button (the §15 nav lock — STOP or
  // run-end are the only ways off).
  lv_obj_t *header = lv_obj_create(parent_);
  theme::apply_panel(header);
  lv_obj_set_width(header, lv_pct(100));
  lv_obj_set_height(header, theme::HEADER_H); // slim info bar — no Back (the §15 nav lock), so no
                                              // touch-target height is needed, and the short 2.8"
                                              // landscape needs the vertical room for the readout
  lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_gap(header, theme::PAD_M, 0);
  lv_obj_t *name = lv_label_create(header);
  lv_label_set_text(name, draft_.name);
  lv_obj_set_flex_grow(name, 1);
  lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
  lv_obj_t *badge = lv_label_create(header);
  lv_label_set_text(badge, "RUNNING");
  lv_obj_set_style_text_color(badge, theme::col(theme::ACCENT), 0);
  create_link_banner(parent_);

  // The readout block — takes the vertical slack so the big temp centres and STOP pins to the
  // bottom.
  lv_obj_t *block = lv_obj_create(parent_);
  lv_obj_remove_style_all(block);
  lv_obj_set_width(block, lv_pct(100));
  lv_obj_set_flex_grow(block, 1);
  lv_obj_set_flex_flow(block, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(block, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_remove_flag(block, LV_OBJ_FLAG_CLICKABLE);

  temp_lbl_ = lv_label_create(block);
  lv_label_set_text(temp_lbl_, "--\xC2\xB0");
  lv_obj_set_style_text_font(temp_lbl_, &theme::big_font(), 0);

  setpoint_lbl_ = lv_label_create(block);
  lv_label_set_text(setpoint_lbl_, "starting...");
  theme::apply_caption(setpoint_lbl_);

  phase_lbl_ = lv_label_create(block);
  lv_label_set_text(phase_lbl_, "");
  lv_obj_set_style_text_color(phase_lbl_, theme::col(theme::TEXT), 0);

  door_lbl_ = lv_label_create(parent_);
  lv_label_set_text(door_lbl_, "");
  lv_obj_set_width(door_lbl_, lv_pct(100));
  lv_obj_set_style_text_align(door_lbl_, LV_TEXT_ALIGN_CENTER, 0);

  // Progress bar (§15) — elapsed against the projected total.
  progress_bar_ = lv_bar_create(parent_);
  lv_obj_set_width(progress_bar_, lv_pct(100));
  lv_obj_set_height(progress_bar_, theme::GAP * 3);
  lv_bar_set_range(progress_bar_, 0, 100);
  lv_bar_set_value(progress_bar_, 0, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(progress_bar_, theme::col(theme::ACCENT), LV_PART_INDICATOR);

  // Output indicators.
  lv_obj_t *ind = lv_obj_create(parent_);
  theme::apply_row(ind);
  lv_obj_set_width(ind, lv_pct(100));
  lv_obj_set_height(ind, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(ind, LV_FLEX_FLOW_ROW);
  uv_pill_ = make_indicator(ind, "UV");
  fan_pill_ = make_indicator(ind, "FAN");
  motor_pill_ = make_indicator(ind, "TURNTABLE");

  // STOP — full-width, red, a single tap (§15/§22: immediate, never a hold).
  lv_obj_t *stop = lv_button_create(parent_);
  theme::apply_mode_tile(stop);
  lv_obj_set_style_bg_color(stop, theme::col(theme::FAULT), 0);
  lv_obj_set_width(stop, lv_pct(100));
  lv_obj_set_height(stop, theme::SECONDARY_H);
  lv_obj_t *stop_lbl = lv_label_create(stop);
  lv_label_set_text(stop_lbl, "STOP");
  lv_obj_center(stop_lbl);
  lv_obj_add_event_cb(stop, RunThunks::stop_evt, LV_EVENT_CLICKED, this);
}

// --- Ended page: the terminal outcome (the rich §16 summary + §22 fault modal are C8's) ---

void RunScreen::buildEnded() {
  clearParent();
  configParent();
  // The Running-page widget pointers are gone now — clear them so a stray poll() can't touch them.
  temp_lbl_ = nullptr;
  setpoint_lbl_ = nullptr;
  phase_lbl_ = nullptr;
  progress_bar_ = nullptr;
  uv_pill_ = nullptr;
  fan_pill_ = nullptr;
  motor_pill_ = nullptr;
  door_lbl_ = nullptr;

  const char *heading;
  uint32_t hue;
  switch (outcome_) {
  case RunOutcome::Completed:
    heading = "Run complete";
    hue = theme::IDLE;
    break;
  case RunOutcome::Stopped:
    heading = "Run stopped";
    hue = theme::WARN;
    break;
  case RunOutcome::Fault:
  default:
    heading = "Fault - run ended";
    hue = theme::FAULT;
    break;
  }

  lv_obj_t *spacer_top = lv_obj_create(parent_);
  lv_obj_remove_style_all(spacer_top);
  lv_obj_set_width(spacer_top, lv_pct(100));
  lv_obj_set_flex_grow(spacer_top, 1);
  lv_obj_remove_flag(spacer_top, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t *h = lv_label_create(parent_);
  lv_label_set_text(h, heading);
  lv_obj_set_width(h, lv_pct(100));
  lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(h, &theme::big_font(), 0);
  lv_obj_set_style_text_color(h, theme::col(hue), 0);

  lv_obj_t *sub = lv_label_create(parent_);
  lv_label_set_text(sub, draft_.name);
  lv_obj_set_width(sub, lv_pct(100));
  lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
  theme::apply_caption(sub);

  lv_obj_t *spacer_bot = lv_obj_create(parent_);
  lv_obj_remove_style_all(spacer_bot);
  lv_obj_set_width(spacer_bot, lv_pct(100));
  lv_obj_set_flex_grow(spacer_bot, 1);
  lv_obj_remove_flag(spacer_bot, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t *done = lv_button_create(parent_);
  theme::apply_mode_tile(done);
  lv_obj_set_width(done, lv_pct(100));
  lv_obj_set_height(done, theme::SECONDARY_H);
  lv_obj_t *done_lbl = lv_label_create(done);
  lv_label_set_text(done_lbl, "Done");
  lv_obj_center(done_lbl);
  lv_obj_add_event_cb(done, RunThunks::done_evt, LV_EVENT_CLICKED, this);
}
