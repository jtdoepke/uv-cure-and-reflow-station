#include "confirm_run_screen.h"

#include <cstdio>

#include "hold_button.h"
#include "link_banner.h"
#include "phase_codec.h" // modeToWire (RecipeMode → oven_Mode) for the recency touch
#include "profile_curve.h"
#include "profile_facts.h"
#include "subjects.h"
#include "theme.h"

using protocol::ReliableSender;

// The §19 arm dwell — deliberately long enough that a brush can't start a run, short enough not to
// feel broken. Shared with any future Resume arm (cure HOLD, a later PR).
static constexpr uint32_t kArmHoldMs = 1500;

namespace {
constexpr float kMinPlausibleC = -10.0f; // below → an open/short probe, not a real reading
constexpr float kMaxPlausibleC = 300.0f; // above → out of the oven's physical range
constexpr float kWallSlackC = 30.0f;     // a real probe can't run this far above the chamber walls
} // namespace

struct ConfirmThunks {
  static void back_evt(lv_event_t *e) {
    static_cast<ConfirmRunScreen *>(lv_event_get_user_data(e))->back();
  }
  static void armed(void *ud) { static_cast<ConfirmRunScreen *>(ud)->commit(); }
};

// --- Lifecycle ---

void ConfirmRunScreen::begin(const ProfileDraft &draft, uint32_t session, protocol::CydLink &link,
                             ManagementClient &mgmt, const OvenModel &model) {
  draft_ = draft;
  session_ = session;
  link_ = &link;
  mgmt_ = &mgmt;
  model_ = &model;
  commit_ = Commit::Idle;
  touch_sent_ = false;
  page_ = Page::Review;
  hold_btn_ = nullptr;
  gate_lbl_ = nullptr;

  // Compile once: the recipe to upload AND the peak/duration facts the statement shows. The editor
  // ceiling = the mode's user cap (§24), floored at 0 to mirror the controller's validator (§9).
  const int32_t cap = draft_.mode == RecipeMode::Cure ? lv_subject_get_int(&subj_uv_cap)
                                                      : lv_subject_get_int(&subj_reflow_cap);
  const Caps caps{0.0f, static_cast<float>(cap)};
  recipe_ = compileRecipe(draft_.phases, draft_.phaseCount, draft_.mode, *model_, caps,
                          profile_facts::kDefaultAmbientC, /*id=*/1, /*seq=*/0);
}

void ConfirmRunScreen::render(lv_obj_t *parent) {
  parent_ = parent;
  switch (page_) {
  case Page::Review:
    buildReview();
    break;
  case Page::Starting:
    buildStarting();
    break;
  case Page::Failed:
    buildFailed();
    break;
  }
}

void ConfirmRunScreen::setExitHandler(void (*cb)(void *), void *user_data) {
  on_exit_ = cb;
  exit_ud_ = user_data;
}

void ConfirmRunScreen::setCommitHandler(void (*cb)(void *, const ProfileDraft &), void *user_data) {
  on_commit_ = cb;
  commit_ud_ = user_data;
}

// --- The TC precondition (pure) ---

bool ConfirmRunScreen::tcAttached(const oven_Telemetry &t) {
  const float w = t.work_temp;
  if (!(w == w)) {
    return false; // NaN → open/faulted probe
  }
  if (w < kMinPlausibleC || w > kMaxPlausibleC) {
    return false; // open-circuit sentinel / out of physical range
  }
  float hottest = -1.0e30f;
  const size_t n = t.wall_temp_count <= 4 ? t.wall_temp_count : 4;
  for (size_t i = 0; i < n; ++i) {
    if (t.wall_temp[i] > hottest) {
      hottest = t.wall_temp[i];
    }
  }
  // A real probe can read cooler than the walls (a cold workpiece in a warm chamber), but not
  // implausibly hotter — that is a dangling/miswired probe, not the load.
  if (n > 0 && w > hottest + kWallSlackC) {
    return false;
  }
  return true;
}

// --- Readiness ---

bool ConfirmRunScreen::ready() const {
  if (link_ == nullptr || !recipe_.hardValid) {
    return false;
  }
  if (lv_subject_get_int(&subj_link_state) != LINK_OK) {
    return false;
  }
  if (isReflow()) {
    // Reflow refuses to start on a probe that is not attached and reading like the load.
    return haveTelem() && tcAttached(telem());
  }
  return true; // cure: no TC gate (the UV caution is advisory, not a gate)
}

// --- poll(): live gate + commit machine ---

void ConfirmRunScreen::poll() {
  if (page_ == Page::Review) {
    refreshGate();
  } else if (page_ == Page::Starting) {
    driveCommit();
  }
}

void ConfirmRunScreen::refreshGate() {
  const bool r = ready();
  applyReady(r);
}

void ConfirmRunScreen::applyReady(bool r) {
  if (hold_btn_ != nullptr) {
    if (r) {
      lv_obj_add_flag(hold_btn_, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_remove_state(hold_btn_, LV_STATE_DISABLED);
    } else {
      lv_obj_remove_flag(hold_btn_, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_state(hold_btn_, LV_STATE_DISABLED);
    }
  }
  if (gate_lbl_ != nullptr) { // reflow TC status line
    if (!haveTelem()) {
      std::snprintf(gate_buf_, sizeof(gate_buf_), "Waiting for the controller...");
    } else if (tcAttached(telem())) {
      std::snprintf(gate_buf_, sizeof(gate_buf_), "Probe OK - reads %.0f\xC2\xB0",
                    static_cast<double>(telem().work_temp));
    } else {
      std::snprintf(gate_buf_, sizeof(gate_buf_), "Attach the workpiece probe");
    }
    lv_label_set_text(gate_lbl_, gate_buf_);
    lv_obj_set_style_text_color(gate_lbl_, theme::col(r ? theme::ACCENT : theme::WARN), 0);
  }
}

void ConfirmRunScreen::driveCommit() {
  const ReliableSender::State st = link_->sender().state();
  const auto fail = [&]() {
    commit_ = Commit::Failed;
    page_ = Page::Failed;
    buildFailed();
  };
  switch (commit_) {
  case Commit::Idle:
    if (link_->sender().sendRecipe(recipe_.recipe)) {
      commit_ = Commit::RecipeSent;
    } else {
      fail();
    }
    break;
  case Commit::RecipeSent:
    if (st == ReliableSender::State::Acked) {
      oven_Start s = oven_Start_init_default;
      s.session = session_;
      s.recipe_id = recipe_.recipe.id;
      if (link_->sender().sendStart(s)) {
        commit_ = Commit::StartSent;
      } else {
        fail();
      }
    } else if (st == ReliableSender::State::Nakd || st == ReliableSender::State::Failed) {
      fail();
    }
    break;
  case Commit::StartSent:
    if (st == ReliableSender::State::Acked) {
      // The controller adopted the session — heartbeats may now authorize it (§9).
      link_->heartbeat().setSession(session_);
      link_->heartbeat().setEnable(true);
      // Best-effort recency bump so this profile floats to the top of the picker (§23/C6-PR1); a
      // busy client just skips it (the run does not depend on it).
      if (mgmt_ != nullptr && !touch_sent_) {
        touch_sent_ = mgmt_->requestTouch(phase_codec::modeToWire(draft_.mode), draft_.name);
      }
      commit_ = Commit::Enabled;
      if (on_commit_ != nullptr) {
        on_commit_(commit_ud_, draft_); // → Run screen (C7), handed the authored draft
      }
    } else if (st == ReliableSender::State::Nakd || st == ReliableSender::State::Failed) {
      fail();
    }
    break;
  case Commit::Enabled:
  case Commit::Failed:
    break;
  }
}

// --- Navigation ---

void ConfirmRunScreen::cancel() {
  if (on_exit_ != nullptr) {
    on_exit_(exit_ud_);
  }
}

void ConfirmRunScreen::back() {
  if (page_ == Page::Failed) {
    page_ = Page::Review;
    commit_ = Commit::Idle;
    buildReview();
    return;
  }
  cancel();
}

void ConfirmRunScreen::commit() {
  if (!ready()) {
    return; // HOLD is disabled in the UI; guard the seam too
  }
  beginCommit();
}

void ConfirmRunScreen::beginCommit() {
  commit_ = Commit::Idle;
  touch_sent_ = false;
  page_ = Page::Starting;
  buildStarting();
}

// --- Shared building blocks (the library/settings idiom) ---

void ConfirmRunScreen::clearParent() {
  lv_obj_clean(parent_);
}

void ConfirmRunScreen::configParent() {
  theme::apply_screen(parent_);
  lv_obj_set_flex_flow(parent_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(parent_, theme::PAD_S, 0);
  lv_obj_set_style_pad_row(parent_, theme::GAP, 0);
}

void ConfirmRunScreen::buildHeader(const char *title) {
  lv_obj_t *header = lv_obj_create(parent_);
  theme::apply_panel(header);
  lv_obj_set_width(header, lv_pct(100));
  lv_obj_set_height(header, theme::SECONDARY_H);
  lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_gap(header, theme::PAD_M, 0);

  lv_obj_t *back = lv_button_create(header);
  theme::apply_secondary(back);
  lv_obj_set_height(back, lv_pct(100));
  lv_obj_t *back_label = lv_label_create(back);
  lv_label_set_text(back_label, LV_SYMBOL_LEFT " Back");
  lv_obj_center(back_label);
  lv_obj_add_event_cb(back, ConfirmThunks::back_evt, LV_EVENT_CLICKED, this);

  lv_obj_t *title_label = lv_label_create(header);
  lv_label_set_text(title_label, title);
  lv_obj_set_flex_grow(title_label, 1);
  lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);

  create_link_banner(parent_);
}

// --- Review page: the feasibility graph + safety precondition + HOLD-to-start ---
//
// This is the ONE page between picking a profile and running it (§19): the picker hands the run
// draft straight here, so the preview graph lives here (not duplicated in a picker detail + a setup
// page). The only forward control is the deliberate HOLD-to-start; the header Back cancels.

void ConfirmRunScreen::buildReview() {
  clearParent();
  configParent();
  // The graph + precondition + HOLD can exceed the short 2.8" landscape, so scroll (apply_screen
  // cleared the flag); a no-op on the tall portrait, where the graph flex-grows to fill instead.
  lv_obj_add_flag(parent_, LV_OBJ_FLAG_SCROLLABLE);
  buildHeader(draft_.name); // the profile being started is the title

  const bool fahrenheit = lv_subject_get_int(&subj_units) != 0;
  const float amb = profile_facts::kDefaultAmbientC;

  // The feasibility preview (requested vs achievable) over the run draft — the same block the
  // library detail draws, now the single graph for the whole run flow.
  profile_facts::CurvePoint req[profile_facts::kMaxCurvePoints];
  profile_facts::CurvePoint over[profile_facts::kMaxCurvePoints];
  float bounds[profile_facts::kMaxCurvePhases];
  profile_facts::TimeSpan uv[kMaxPhases];
  const size_t nr =
      profile_facts::sampleCurve(draft_.phases, draft_.phaseCount, draft_.mode, *model_,
                                 /*achievable=*/false, amb, req, profile_facts::kMaxCurvePoints);
  const size_t no =
      profile_facts::sampleOvershoot(draft_.phases, draft_.phaseCount, draft_.mode, *model_, amb,
                                     over, profile_facts::kMaxCurvePoints);
  const size_t nb =
      profile_facts::samplePhaseBoundaries(draft_.phases, draft_.phaseCount, draft_.mode, *model_,
                                           amb, bounds, profile_facts::kMaxCurvePhases);
  const size_t nuv = profile_facts::sampleUvSpans(draft_.phases, draft_.phaseCount, draft_.mode,
                                                  *model_, amb, uv, kMaxPhases);
  char namebuf[profile_facts::kMaxCurvePhases][kPhaseNameCap];
  const char *names[profile_facts::kMaxCurvePhases];
  const size_t authored = draft_.phaseCount < profile_facts::kMaxCurvePhases
                              ? draft_.phaseCount
                              : profile_facts::kMaxCurvePhases;
  for (size_t i = 0; i < authored; ++i) {
    std::strncpy(namebuf[i], draft_.phases[i].name, kPhaseNameCap - 1);
    namebuf[i][kPhaseNameCap - 1] = '\0';
    names[i] = namebuf[i];
  }
  ProfileCurveData cd;
  cd.requested = req;
  cd.n_requested = nr;
  cd.overshoot = over;
  cd.n_overshoot = no;
  cd.boundaries = bounds;
  cd.n_boundaries = nb;
  cd.uv_spans = uv;
  cd.n_uv_spans = nuv;
  cd.phase_names = names;
  cd.n_phase_names = authored;
  cd.cool_start = (nb > authored && authored >= 1) ? bounds[authored - 1] : -1.0f;
  cd.uncalibrated = false;
  // Keep the curve at its intrinsic height (panel::H/4 + legend) — flex-growing it to fill the tall
  // portrait stretches the fixed-aspect instrument vertically. A spacer below pins HOLD to the
  // bottom instead.
  create_profile_curve(parent_, cd);

  // Key facts (peak · duration · phases) as a caption under the graph.
  const profile_facts::ProfileFacts f =
      profile_facts::computeFacts(draft_.phases, draft_.phaseCount, draft_.mode, *model_, amb);
  char peak[16];
  char dur[16];
  char facts[40];
  profile_facts::formatPeak(f.peakC, fahrenheit, peak, sizeof(peak));
  profile_facts::formatDuration(f.totalSeconds, dur, sizeof(dur));
  // Peak · duration only — the graph shows the phases, and a "N phases" tail wraps on portrait.
  std::snprintf(facts, sizeof(facts), "%s \xC2\xB7 %s", peak, dur);
  lv_obj_t *facts_lbl = lv_label_create(parent_);
  lv_label_set_text(facts_lbl, facts);
  lv_obj_set_width(facts_lbl, lv_pct(100));
  lv_obj_set_style_text_align(facts_lbl, LV_TEXT_ALIGN_CENTER, 0);
  theme::apply_caption(facts_lbl);

  // Safety precondition.
  if (isReflow()) {
    // The reflow precondition: the workpiece TC must be attached and reading plausibly. gate_lbl_
    // is refreshed live by refreshGate(); its initial text is set by applyReady() below.
    gate_lbl_ = lv_label_create(parent_);
    lv_obj_set_width(gate_lbl_, lv_pct(100));
    lv_obj_set_style_text_align(gate_lbl_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(gate_lbl_, LV_LABEL_LONG_WRAP);
  } else {
    // Cure has no precondition line: the UV array is filtered at the door window and the door
    // latches cut the light when it opens, so no eye-hazard caution is needed.
    gate_lbl_ = nullptr;
  }

  // A flex-grow spacer takes the slack so the graph keeps its natural aspect and HOLD sits at the
  // bottom (thumb reach). On the short 2.8" landscape it collapses to nothing and the page scrolls.
  lv_obj_t *spacer = lv_obj_create(parent_);
  lv_obj_remove_style_all(spacer);
  lv_obj_set_width(spacer, lv_pct(100));
  lv_obj_set_flex_grow(spacer, 1);
  lv_obj_remove_flag(spacer, LV_OBJ_FLAG_CLICKABLE);

  // The single forward control: HOLD to start (the header Back cancels — no separate button).
  HoldButton hold =
      create_hold_button(parent_, "HOLD to start", kArmHoldMs, ConfirmThunks::armed, this);
  hold_btn_ = hold.root;

  applyReady(ready()); // set the initial HOLD enable + gate line
}

// --- Starting page: the §9 handshake in flight ---

void ConfirmRunScreen::buildStarting() {
  clearParent();
  configParent();
  buildHeader("Starting");
  hold_btn_ = nullptr;
  gate_lbl_ = nullptr;

  lv_obj_t *lbl = lv_label_create(parent_);
  lv_label_set_text(lbl, "Starting the run...");
  lv_obj_set_flex_grow(lbl, 1);
  lv_obj_set_width(lbl, lv_pct(100));
  lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
  theme::apply_caption(lbl);
}

// --- Failed page: a Nak/timeout on Recipe or Start ---

void ConfirmRunScreen::buildFailed() {
  clearParent();
  configParent();
  buildHeader("Couldn't start");
  hold_btn_ = nullptr;
  gate_lbl_ = nullptr;

  lv_obj_t *lbl = lv_label_create(parent_);
  lv_label_set_text(lbl, "The controller refused or did not answer the start.\nCheck the link and "
                         "try again.");
  lv_obj_set_width(lbl, lv_pct(100));
  lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_flex_grow(lbl, 1);
  lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
  theme::apply_caption(lbl);

  lv_obj_t *retry = lv_button_create(parent_);
  theme::apply_mode_tile(retry);
  lv_obj_set_width(retry, lv_pct(100));
  lv_obj_set_height(retry, theme::SECONDARY_H);
  lv_obj_t *retry_lbl = lv_label_create(retry);
  lv_label_set_text(retry_lbl, "Back");
  lv_obj_center(retry_lbl);
  lv_obj_add_event_cb(retry, ConfirmThunks::back_evt, LV_EVENT_CLICKED, this);
}
