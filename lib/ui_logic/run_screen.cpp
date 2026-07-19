#include "run_screen.h"

#include <cstdio>

#include "codec.h"       // protocol::wireEnum — reading an untrusted enum field without UB
#include "fault_table.h" // §16's outcome "(+ cause)" — the same table the §22 overlay reads
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
  static void again_evt(lv_event_t *e) {
    static_cast<RunScreen *>(lv_event_get_user_data(e))->runAgain();
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
  fit_ = RunFitResult{};
  actual_last_ = -1;
  deviated_ = false;

  // Arm the tracker over the same compile inputs the recipe was uploaded with (§15): the mode's
  // user cap (§24) floored at 0, the shared default ambient — so its segment→phase map matches the
  // recipe the controller is executing.
  const int32_t cap = draft_.mode == RecipeMode::Cure ? lv_subject_get_int(&subj_uv_cap)
                                                      : lv_subject_get_int(&subj_reflow_cap);
  const Caps caps{0.0f, static_cast<float>(cap)};
  tracker_.begin(draft_, *model_, caps, profile_facts::kDefaultAmbientC, lv_tick_get());
  prepareCurve();
}

// Sample the projected trajectory once per run. The y-range is widened as measured points arrive
// (recordActual) rather than fixed here: the projection is by definition what the oven was
// supposed to do, and a run that overshoots it — the known ramp-into-hold overshoot — would draw
// its most interesting feature clipped against the top of the plot.
void RunScreen::prepareCurve() {
  const float total = tracker_.totalSeconds();
  float lo = 1.0e30f;
  float hi = -1.0e30f;
  for (uint16_t i = 0; i < kCurvePoints; ++i) {
    const float ts =
        total > 0.0f ? static_cast<float>(i) / static_cast<float>(kCurvePoints - 1) * total : 0.0f;
    proj_[i] = tracker_.projectedAt(ts);
    actual_[i] = 0.0f;
    if (proj_[i] < lo) {
      lo = proj_[i];
    }
    if (proj_[i] > hi) {
      hi = proj_[i];
    }
  }
  y_lo_ = static_cast<int32_t>(lo) - 5;
  y_hi_ = static_cast<int32_t>(hi) + 10;
}

void RunScreen::recordActual(float frac01, float valueC) {
  if (!(valueC == valueC)) { // NaN — a faulted TC; hold the last point rather than plot a hole
    return;
  }
  if (frac01 < 0.0f) {
    frac01 = 0.0f;
  } else if (frac01 > 1.0f) {
    frac01 = 1.0f;
  }
  int32_t idx = static_cast<int32_t>(frac01 * static_cast<float>(kCurvePoints - 1) + 0.5f);
  if (idx < 0) {
    idx = 0;
  } else if (idx >= static_cast<int32_t>(kCurvePoints)) {
    idx = kCurvePoints - 1;
  }
  // Same gap-fill rule as run_curve_push_actual, so a replay reproduces the live trace exactly.
  const int32_t from = (idx > actual_last_) ? actual_last_ + 1 : idx;
  for (int32_t j = from; j <= idx; ++j) {
    actual_[j] = valueC;
  }
  if (idx > actual_last_) {
    actual_last_ = idx;
  }

  // Widen the plot to hold a measured excursion outside the projection.
  const int32_t v = static_cast<int32_t>(valueC);
  if (v - 5 < y_lo_) {
    y_lo_ = v - 5;
  }
  if (v + 10 > y_hi_) {
    y_hi_ = v + 10;
  }
}

void RunScreen::buildCurve() {
  run_curve_ = create_run_curve(parent_, proj_, kCurvePoints, y_lo_, y_hi_);
  run_curve_set_actual(run_curve_, actual_, actual_last_, deviated_);
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

void RunScreen::setRunAgainHandler(void (*cb)(void *), void *user_data) {
  on_again_ = cb;
  again_ud_ = user_data;
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
    // Append the measured point to the chart, amber while off-plan (§16 shares the cue object).
    // The retained buffer is written first and unconditionally: the summary redraws from it after
    // the live widget is gone, so it must not depend on the widget existing.
    deviated_ = tracker_.deviating();
    recordActual(tracker_.progress01(), controlTempC(t));
    run_curve_push_actual(run_curve_, tracker_.progress01(), controlTempC(t), deviated_);
  }
  refresh(t, ours);

  if (!ours) {
    return; // our run's frames haven't started arriving yet — nothing terminal to read
  }
  if (t.run_state == oven_RunState_RUN_STATE_RUNNING) {
    saw_running_ = true;
  }
  // Read the untrusted enum field as its raw wire integer: nanopb stores the decoded varint
  // verbatim, and an enum-typed load of an out-of-enum value is UB (protocol::wireEnum).
  const fault_table::FaultCodeWire fault_wire = protocol::wireEnum(t.fault_code);
  if (fault_wire != oven_FaultCode_FAULT_NONE || t.run_state == oven_RunState_RUN_STATE_FAULT) {
    fault_code_ = fault_wire; // may be FAULT_NONE if only run_state said so — formatTitle copes
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
    // One line: phase N/M · ETA. The setpoint is the chart's projected ghost, so it needs no text
    // here; short forms ("2/4") keep it on one line on the narrow 2.8".
    char dur[16];
    profile_facts::formatDuration(tracker_.etaSeconds(), dur, sizeof(dur));
    if (tracker_.inCooldown()) {
      std::snprintf(phase_buf_, sizeof(phase_buf_), "Cooling \xC2\xB7 %s left", dur);
    } else {
      std::snprintf(phase_buf_, sizeof(phase_buf_), "%s %u/%u \xC2\xB7 %s left",
                    tracker_.phaseName(), static_cast<unsigned>(tracker_.phaseOrdinal()),
                    static_cast<unsigned>(tracker_.phaseCount()), dur);
    }
    lv_label_set_text(phase_lbl_, phase_buf_);
  }
  // Status cue: door open takes priority (§22 caution), then the off-plan deviation cue (§16), else
  // on-plan. The chart's trace colour echoes the deviation; the word is the colour-blind read.
  if (cue_lbl_ != nullptr) {
    if (t.door_open) {
      lv_label_set_text(cue_lbl_, LV_SYMBOL_WARNING " DOOR OPEN");
      lv_obj_set_style_text_color(cue_lbl_, theme::col(theme::WARN), 0);
    } else if (tracker_.deviating()) {
      lv_label_set_text(cue_lbl_, "Off plan - check the oven");
      lv_obj_set_style_text_color(cue_lbl_, theme::col(theme::WARN), 0);
    } else {
      lv_label_set_text(cue_lbl_, "On plan");
      lv_obj_set_style_text_color(cue_lbl_, theme::col(theme::TEXT_DIM), 0);
    }
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
}

void RunScreen::endRun(RunOutcome outcome) {
  outcome_ = outcome;
  fit_ = tracker_.finish(outcome, lv_tick_get()); // the §16 fit the summary renders
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

void RunScreen::runAgain() {
  if (on_again_ != nullptr) {
    on_again_(again_ud_);
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

  // Readout band: the headline control temperature (big) over one compact info line (target · phase
  // · ETA). Centred, content-height, so the chart below takes the slack — and full-width lines so
  // nothing wraps on the narrow 2.8".
  lv_obj_t *band = lv_obj_create(parent_);
  lv_obj_remove_style_all(band);
  lv_obj_set_width(band, lv_pct(100));
  lv_obj_set_height(band, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(band, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(band, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_remove_flag(band, LV_OBJ_FLAG_CLICKABLE);

  temp_lbl_ = lv_label_create(band);
  lv_label_set_text(temp_lbl_, "--\xC2\xB0");
  lv_obj_set_style_text_font(temp_lbl_, &theme::big_font(), 0);

  // setpoint_lbl_ is left unused (its target is folded into phase_lbl_); refresh() guards on null.
  setpoint_lbl_ = nullptr;
  phase_lbl_ = lv_label_create(band);
  lv_label_set_text(phase_lbl_, "starting...");
  lv_obj_set_width(phase_lbl_, lv_pct(100));
  lv_label_set_long_mode(phase_lbl_, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(phase_lbl_, LV_TEXT_ALIGN_CENTER, 0);
  theme::apply_caption(phase_lbl_);

  // The projected-vs-actual chart (C7b) — the star of the screen; grows to fill the slack. Replays
  // the retained samples, so a rebuild mid-run comes back with its history rather than a blank
  // trace.
  buildCurve();

  // Status cue (§15/§16): door open (priority) / off-plan / on-plan. The chart's actual trace also
  // turns amber off-plan, but the word carries it for a colour-blind read.
  cue_lbl_ = lv_label_create(parent_);
  lv_label_set_text(cue_lbl_, "");
  lv_obj_set_width(cue_lbl_, lv_pct(100));
  lv_obj_set_style_text_align(cue_lbl_, LV_TEXT_ALIGN_CENTER, 0);

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

// --- Ended page = the §16 Run Summary ---
//
// Reports the outcome and surfaces calibration drift. The §22 fault MODAL is a separate overlay
// that draws over this page; here a Fault outcome is just a badge + cause, because by the time
// this page exists the machine is already safed and this is the run's record, not an alarm.

namespace {
const char *verdictWord(FitVerdict v) {
  switch (v) {
  case FitVerdict::Good:
    return "Good";
  case FitVerdict::Fair:
    return "Fair";
  case FitVerdict::Poor:
  default:
    return "Poor";
  }
}
} // namespace

void RunScreen::buildEnded() {
  clearParent();
  configParent();
  // The one screen in the app that scrolls. Every other page is designed to fit, but this one's
  // height is DATA-dependent — the drift advisory is a paragraph that is present or absent — and on
  // the 240 px landscape the worst case genuinely overflows. Scrolling keeps the action row
  // reachable instead of clipped; without it, a run that drew an advisory would strand the operator
  // on a page whose only two buttons are off the bottom edge.
  lv_obj_add_flag(parent_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(parent_, LV_DIR_VER);
  // The Running-page widget pointers are gone now — clear them so a stray poll()/push can't touch
  // them (the chart's actual_y buffer is freed with the widget tree; ours is retained separately).
  temp_lbl_ = nullptr;
  setpoint_lbl_ = nullptr;
  phase_lbl_ = nullptr;
  cue_lbl_ = nullptr;
  uv_pill_ = nullptr;
  fan_pill_ = nullptr;
  motor_pill_ = nullptr;
  run_curve_ = RunCurve{};

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

  // Outcome badge + provenance. One slim band, so the curve keeps the vertical room.
  lv_obj_t *header = lv_obj_create(parent_);
  theme::apply_panel(header);
  lv_obj_set_width(header, lv_pct(100));
  lv_obj_set_height(header, theme::HEADER_H);
  lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_gap(header, theme::PAD_M, 0);
  lv_obj_t *h = lv_label_create(header);
  lv_label_set_text(h, heading);
  lv_obj_set_style_text_color(h, theme::col(hue), 0);
  lv_obj_t *sub = lv_label_create(header);
  lv_label_set_text(sub, draft_.name);
  lv_obj_set_flex_grow(sub, 1);
  lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_RIGHT, 0);
  theme::apply_caption(sub);

  // §16's "(+ cause)" — the plain-language fault title, from the same table the §22 overlay reads.
  // formatTitle, NOT faultInfo().title: an unrecognized code (and FAULT_NONE, which a run_state-
  // only FAULT frame leaves us holding) has a null title in the table, and formatTitle is the
  // accessor that turns that into §22's generic "Fault <n> - oven safed to a safe state".
  if (outcome_ == RunOutcome::Fault) {
    char cause_buf[64];
    fault_table::formatTitle(fault_code_, cause_buf, sizeof(cause_buf));
    lv_obj_t *cause = lv_label_create(parent_);
    lv_label_set_text(cause, cause_buf);
    lv_obj_set_width(cause, lv_pct(100));
    lv_label_set_long_mode(cause, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(cause, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(cause, theme::col(theme::FAULT), 0);
  }

  // §16's "full-run projected-vs-actual overlay (both curves complete)" — the same widget the live
  // page used, rebuilt from the retained samples. It is the primary content, so it gets a MINIMUM
  // height: it is the only flex-grow child, and on the short landscape the content-height siblings
  // (badge + stats + a six-line advisory + the action row) otherwise sum past the panel and squeeze
  // the plot to zero — the chart vanishing is exactly the wrong thing to lose on a results screen.
  buildCurve();
  lv_obj_set_style_min_height(run_curve_.root, panel::H / 4, 0);

  // §16's fit verdict + key numbers. Only a completed run has them ("abort/fault skip it — data
  // incomplete"); say so rather than leaving a hole where the numbers would be.
  // Verdict + deviation on one line, phase outcomes on the next. Two labels rather than one wrapped
  // string on both panels: combined it exceeds the line at either width, and letting it wrap splits
  // it mid-figure ("2/5" on one row, "on target" on the next). Same height, honest break.
  lv_obj_t *stats = lv_label_create(parent_);
  if (fit_.computed) {
    std::snprintf(stats_buf_, sizeof(stats_buf_),
                  "%s \xC2\xB7 max %.1f\xC2\xB0 \xC2\xB7 rms %.1f\xC2\xB0",
                  verdictWord(fit_.verdict), static_cast<double>(fit_.runQuality.maxAbsC),
                  static_cast<double>(fit_.runQuality.rmsC));
    lv_label_set_text(stats, stats_buf_);
  } else {
    lv_label_set_text(stats, "No fit - the run did not complete");
  }
  lv_obj_set_width(stats, lv_pct(100));
  lv_label_set_long_mode(stats, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(stats, LV_TEXT_ALIGN_CENTER, 0);
  theme::apply_caption(stats);

  if (fit_.computed) {
    const int reached =
        static_cast<int>(fit_.phaseCount) - static_cast<int>(fit_.phasesMissedTarget);
    lv_obj_t *phases = lv_label_create(parent_);
    if (fit_.phasesShortHold > 0) {
      lv_label_set_text_fmt(phases, "%d/%u on target \xC2\xB7 %u short hold%s", reached,
                            static_cast<unsigned>(fit_.phaseCount),
                            static_cast<unsigned>(fit_.phasesShortHold),
                            fit_.phasesShortHold == 1 ? "" : "s");
    } else {
      lv_label_set_text_fmt(phases, "%d/%u on target", reached,
                            static_cast<unsigned>(fit_.phaseCount));
    }
    lv_obj_set_width(phases, lv_pct(100));
    lv_label_set_long_mode(phases, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(phases, LV_TEXT_ALIGN_CENTER, 0);
    theme::apply_caption(phases);
  }

  // §16's calibration-drift advisory — an INLINE AMBER BANNER, never the red modal (§22).
  if (fit_.computed && fit_.advisory) {
    lv_obj_t *adv = lv_obj_create(parent_);
    theme::apply_alert(adv, theme::WARN);
    lv_obj_set_width(adv, lv_pct(100));
    lv_obj_set_height(adv, LV_SIZE_CONTENT);
    // Bounded, and scrolls its own overflow. The advisory is a paragraph and the panel may be only
    // 240 px tall; letting it run to content height pushed the chart and the action buttons off the
    // screen. Capped it stays fully readable (by scrolling) without evicting anything. The tall
    // portrait panel affords a third of its height, which fits the whole paragraph unscrolled; the
    // short landscape gets a quarter, which is what keeps the action row on screen there.
    lv_obj_set_style_max_height(adv, panel::kPortrait ? panel::H / 3 : panel::H / 4, 0);
    lv_obj_t *txt = lv_label_create(adv);
    // The ⚠ is the VIEW's to add (theme.h: the alert helpers style the container; the redundant
    // glyph + word are the caller's). LV_SYMBOL_WARNING is Font Awesome 0xF071, which the body
    // fonts carry — a literal U+26A0 in the advisory string would not be, and big_font() carries
    // no symbols at all.
    lv_label_set_text_fmt(txt, LV_SYMBOL_WARNING " %s", advisoryText(fit_.cause));
    lv_obj_set_width(txt, lv_pct(100));
    lv_label_set_long_mode(txt, LV_LABEL_LONG_WRAP);
  }

  // §16's actions. "Run again" never re-energizes directly — it routes back through Confirm (§19).
  lv_obj_t *actions = lv_obj_create(parent_);
  theme::apply_row(actions);
  lv_obj_set_width(actions, lv_pct(100));
  lv_obj_set_height(actions, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_gap(actions, theme::GAP, 0);

  lv_obj_t *again = lv_button_create(actions);
  theme::apply_secondary(again);
  lv_obj_set_flex_grow(again, 1);
  lv_obj_set_height(again, theme::SECONDARY_H);
  lv_obj_t *again_lbl = lv_label_create(again);
  lv_label_set_text(again_lbl, "Run again");
  lv_obj_center(again_lbl);
  lv_obj_add_event_cb(again, RunThunks::again_evt, LV_EVENT_CLICKED, this);

  lv_obj_t *done = lv_button_create(actions);
  theme::apply_mode_tile(done);
  lv_obj_set_flex_grow(done, 1);
  lv_obj_set_height(done, theme::SECONDARY_H);
  lv_obj_t *done_lbl = lv_label_create(done);
  lv_label_set_text(done_lbl, "Home");
  lv_obj_center(done_lbl);
  lv_obj_add_event_cb(done, RunThunks::done_evt, LV_EVENT_CLICKED, this);
}
