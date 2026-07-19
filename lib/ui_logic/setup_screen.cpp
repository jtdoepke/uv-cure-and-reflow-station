#include "setup_screen.h"

#include <cstdio>
#include <cstring>

#include "link_banner.h" // shared "Controller not responding" banner (§9/§14)
#include "panel.h"       // panel::kPortrait — geometry, never a board identity
#include "profile_curve.h"
#include "profile_facts.h"
#include "recipe_compiler.h"
#include "subjects.h"
#include "theme.h"

// Captureless thunks — a friend of SetupScreen so they can reach its private navigation methods
// (the codebase's single-void*-user_data idiom, no std::function).
struct SetupThunks {
  static void back_evt(lv_event_t *e) {
    static_cast<SetupScreen *>(lv_event_get_user_data(e))->back();
  }
  static void load_evt(lv_event_t *e) {
    static_cast<SetupScreen *>(lv_event_get_user_data(e))->onLoad();
  }
  static void edit_evt(lv_event_t *e) {
    static_cast<SetupScreen *>(lv_event_get_user_data(e))->onEdit();
  }
  static void saveas_evt(lv_event_t *e) {
    static_cast<SetupScreen *>(lv_event_get_user_data(e))->onSaveAs();
  }
  static void start_evt(lv_event_t *e) {
    static_cast<SetupScreen *>(lv_event_get_user_data(e))->onStart();
  }
};

// --- Lifecycle ---

void SetupScreen::enterMode(RecipeMode mode, const OvenModel &model) {
  mode_ = mode;
  model_ = &model;
  draft_ = ProfileDraft{};
  draft_.mode = mode;
  have_draft_ = false;
  page_ = Page::Empty;
}

void SetupScreen::setDraft(const ProfileDraft &draft) {
  draft_ = draft;
  if (draft_.phaseCount > kMaxPhases) {
    draft_.phaseCount = kMaxPhases;
  }
  mode_ = draft_.mode; // the picked/edited profile's mode is authoritative
  have_draft_ = true;
  page_ = Page::Loaded;
}

void SetupScreen::render(lv_obj_t *parent) {
  parent_ = parent;
  if (have_draft_) {
    page_ = Page::Loaded;
    buildLoaded();
  } else {
    page_ = Page::Empty;
    buildEmpty();
  }
}

void SetupScreen::setExitHandler(void (*cb)(void *), void *user_data) {
  on_exit_ = cb;
  exit_ud_ = user_data;
}

void SetupScreen::publishNav(int nav_request) {
  lv_subject_set_int(&subj_nav_request, nav_request);
}

// --- Navigation / actions ---

void SetupScreen::back() {
  if (on_exit_ != nullptr) {
    on_exit_(exit_ud_);
  }
}

void SetupScreen::onLoad() {
  publishNav(NAV_SETUP_PICK);
}
void SetupScreen::onEdit() {
  publishNav(NAV_SETUP_EDIT);
}
void SetupScreen::onSaveAs() {
  publishNav(NAV_SETUP_SAVE_AS);
}

void SetupScreen::onStart() {
  if (!ready()) {
    return; // Start is disabled in the UI; guard the seam too
  }
  publishNav(NAV_SETUP_START); // → Confirm (C6b)
}

// --- Readiness ---

bool SetupScreen::compileValid() const {
  if (!have_draft_) {
    return false;
  }
  // Editor ceiling = the mode's user max-temp setting (published cross-screen, §24); floor at 0 so
  // an accepted compile mirrors the controller's RecipeValidator (§9), exactly as the editor does.
  const int32_t cap = mode_ == RecipeMode::Cure ? lv_subject_get_int(&subj_uv_cap)
                                                : lv_subject_get_int(&subj_reflow_cap);
  const Caps caps{0.0f, static_cast<float>(cap)};
  const CompileResult cr = compileRecipe(draft_.phases, draft_.phaseCount, mode_, *model_, caps,
                                         profile_facts::kDefaultAmbientC, /*id=*/0, /*seq=*/0);
  return cr.hardValid;
}

bool SetupScreen::ready() const {
  // Door-open joins this gate in C7/PR5, once the controller reports door state (subj_door_open).
  return compileValid() && lv_subject_get_int(&subj_link_state) == LINK_OK;
}

// --- Shared building blocks (the library/settings idiom) ---

void SetupScreen::clearParent() {
  lv_obj_clean(parent_);
}

void SetupScreen::configParent() {
  theme::apply_screen(parent_);
  lv_obj_set_flex_flow(parent_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(parent_, theme::PAD_S, 0);
  lv_obj_set_style_pad_row(parent_, theme::GAP, 0);
}

void SetupScreen::buildHeader(const char *title) {
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
  lv_obj_add_event_cb(back, SetupThunks::back_evt, LV_EVENT_CLICKED, this);

  lv_obj_t *title_label = lv_label_create(header);
  lv_label_set_text(title_label, title);
  lv_obj_set_flex_grow(title_label, 1);
  lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);

  // A run is composed against the controller (the profile is fetched, the link must be up to
  // start), so surface a dropped link here as everywhere.
  create_link_banner(parent_);
}

// --- Empty page: no profile chosen yet ---

void SetupScreen::buildEmpty() {
  clearParent();
  configParent();
  buildHeader(mode_ == RecipeMode::Cure ? "UV cure setup" : "Reflow setup");

  // One big central call to action. Load is link-gated like Home's run tiles (it fetches the
  // library from the controller, §9) — greyed + non-clickable when the link is down.
  lv_obj_t *spacer_top = lv_obj_create(parent_);
  lv_obj_remove_style_all(spacer_top);
  lv_obj_set_width(spacer_top, lv_pct(100));
  lv_obj_set_flex_grow(spacer_top, 1);
  lv_obj_remove_flag(spacer_top, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t *load = lv_button_create(parent_);
  theme::apply_mode_tile(load);
  lv_obj_set_width(load, lv_pct(100));
  lv_obj_set_height(load, theme::SECONDARY_H);
  lv_obj_t *load_lbl = lv_label_create(load);
  lv_label_set_text(load_lbl, "Load a profile");
  lv_obj_center(load_lbl);
  lv_obj_add_event_cb(load, SetupThunks::load_evt, LV_EVENT_CLICKED, this);
  lv_obj_bind_flag_if_eq(load, &subj_link_state, LV_OBJ_FLAG_CLICKABLE, LINK_OK);
  lv_obj_bind_state_if_not_eq(load, &subj_link_state, LV_STATE_DISABLED, LINK_OK);

  lv_obj_t *hint = lv_label_create(parent_);
  lv_label_set_text(hint, mode_ == RecipeMode::Cure ? "Pick a cure profile to run"
                                                    : "Pick a reflow profile to run");
  lv_obj_set_width(hint, lv_pct(100));
  lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
  theme::apply_caption(hint);

  lv_obj_t *spacer_bot = lv_obj_create(parent_);
  lv_obj_remove_style_all(spacer_bot);
  lv_obj_set_width(spacer_bot, lv_pct(100));
  lv_obj_set_flex_grow(spacer_bot, 1);
  lv_obj_remove_flag(spacer_bot, LV_OBJ_FLAG_CLICKABLE);
}

// --- Loaded page: a profile is chosen — preview + actions + Start ---

namespace {
// One secondary action button in the Load/Edit/Save-as row. `this_screen` is the click user_data.
lv_obj_t *action_button(lv_obj_t *row, const char *text, lv_event_cb_t cb, void *this_screen,
                        bool link_gated) {
  lv_obj_t *btn = lv_button_create(row);
  theme::apply_secondary(btn);
  lv_obj_set_flex_grow(btn, 1);
  lv_obj_set_height(btn, lv_pct(100));
  lv_obj_t *label = lv_label_create(btn);
  lv_label_set_text(label, text);
  lv_obj_center(label);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, this_screen);
  if (link_gated) {
    // Load fetches the library and Save-as writes it — both need the link. Edit is a local
    // working-copy tweak (no controller round-trip), so it is NOT gated.
    lv_obj_bind_flag_if_eq(btn, &subj_link_state, LV_OBJ_FLAG_CLICKABLE, LINK_OK);
    lv_obj_bind_state_if_not_eq(btn, &subj_link_state, LV_STATE_DISABLED, LINK_OK);
  }
  return btn;
}
} // namespace

void SetupScreen::buildLoaded() {
  clearParent();
  configParent();
  // The loaded page can exceed the short 2.8" landscape (header + preview + facts + two button rows
  // + Start), so let it scroll — configParent's apply_screen cleared the flag. On the tall 3.5"
  // portrait everything fits, so the flag is a harmless no-op there.
  lv_obj_add_flag(parent_, LV_OBJ_FLAG_SCROLLABLE);
  buildHeader(mode_ == RecipeMode::Cure ? "UV cure setup" : "Reflow setup");

  const bool fahrenheit = lv_subject_get_int(&subj_units) != 0;

  // Provenance: which profile this run came from (name + a stock marker).
  std::snprintf(prov_buf_, sizeof(prov_buf_), "%s%s", draft_.name, draft_.stock ? "  (stock)" : "");
  lv_obj_t *prov = lv_label_create(parent_);
  lv_label_set_text(prov, prov_buf_);
  lv_obj_set_width(prov, lv_pct(100));
  lv_label_set_long_mode(prov, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_font(prov, &theme::big_font(), 0);

  // Read-only §12 feasibility preview (requested vs achievable) over the run draft — the same block
  // the library detail draws, keyed on the draft's phases. Local arrays — the widget copies.
  profile_facts::CurvePoint req[profile_facts::kMaxCurvePoints];
  profile_facts::CurvePoint over[profile_facts::kMaxCurvePoints];
  float bounds[profile_facts::kMaxCurvePhases];
  profile_facts::TimeSpan uv[kMaxPhases];
  const float amb = profile_facts::kDefaultAmbientC;
  const size_t nr =
      profile_facts::sampleCurve(draft_.phases, draft_.phaseCount, mode_, *model_,
                                 /*achievable=*/false, amb, req, profile_facts::kMaxCurvePoints);
  const size_t no = profile_facts::sampleOvershoot(draft_.phases, draft_.phaseCount, mode_, *model_,
                                                   amb, over, profile_facts::kMaxCurvePoints);
  const size_t nb =
      profile_facts::samplePhaseBoundaries(draft_.phases, draft_.phaseCount, mode_, *model_, amb,
                                           bounds, profile_facts::kMaxCurvePhases);
  const size_t nuv = profile_facts::sampleUvSpans(draft_.phases, draft_.phaseCount, mode_, *model_,
                                                  amb, uv, kMaxPhases);
  // Phase labels: one per authored phase; the implicit passive cool tail is unlabelled.
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
  const float cool_start = (nb > authored && authored >= 1) ? bounds[authored - 1] : -1.0f;
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
  cd.cool_start = cool_start;
  cd.uncalibrated = false;
  ProfileCurve curve = create_profile_curve(parent_, cd);
  // Portrait has vertical slack — let the preview grow to fill it (the 3.5" default). On the short
  // landscape the widget keeps its intrinsic height and the page scrolls; growing it there would
  // squeeze it to nothing against the fixed-height button rows.
  if (panel::kPortrait) {
    lv_obj_set_flex_grow(curve.root, 1);
  }

  // Key facts: "peak 245° · ~6:10 · 4 phases".
  const profile_facts::ProfileFacts f =
      profile_facts::computeFacts(draft_.phases, draft_.phaseCount, mode_, *model_, amb);
  char peak[16];
  char dur[16];
  profile_facts::formatPeak(f.peakC, fahrenheit, peak, sizeof(peak));
  profile_facts::formatDuration(f.totalSeconds, dur, sizeof(dur));
  std::snprintf(facts_buf_, sizeof(facts_buf_), "%s \xC2\xB7 %s \xC2\xB7 %u phases", peak, dur,
                static_cast<unsigned>(f.phaseCount));
  lv_obj_t *facts_label = lv_label_create(parent_);
  lv_label_set_text(facts_label, facts_buf_);
  theme::apply_caption(facts_label);

  // Action row: Load (swap profile) · Edit (tweak for this run) · Save as (persist the tweaks).
  lv_obj_t *row = lv_obj_create(parent_);
  theme::apply_row(row);
  lv_obj_set_width(row, lv_pct(100));
  lv_obj_set_height(row, theme::SECONDARY_H);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  action_button(row, "Load", SetupThunks::load_evt, this, /*link_gated=*/true);
  action_button(row, "Edit", SetupThunks::edit_evt, this, /*link_gated=*/false);
  action_button(row, "Save as", SetupThunks::saveas_evt, this, /*link_gated=*/true);

  // Readiness line — why Start is (not) available, in words (never colour alone, §14).
  const bool cv = compileValid();
  const bool link_ok = lv_subject_get_int(&subj_link_state) == LINK_OK;
  const char *msg;
  uint32_t hue;
  if (!cv) {
    msg = "Profile not runnable - Edit to fix";
    hue = theme::WARN;
  } else if (!link_ok) {
    msg = "Controller offline";
    hue = theme::WARN;
  } else {
    msg = "Ready to start";
    hue = theme::ACCENT;
  }
  lv_obj_t *ready_lbl = lv_label_create(parent_);
  lv_label_set_text(ready_lbl, msg);
  lv_obj_set_width(ready_lbl, lv_pct(100));
  lv_obj_set_style_text_align(ready_lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(ready_lbl, theme::col(hue), 0);

  // Start → Confirm (C6b). A hard-invalid profile is always disabled; an otherwise-runnable one is
  // link-gated (re-enables reactively on reconnect), mirroring the editor's Save.
  lv_obj_t *start = lv_button_create(parent_);
  theme::apply_mode_tile(start);
  lv_obj_set_width(start, lv_pct(100));
  lv_obj_set_height(start, theme::SECONDARY_H);
  lv_obj_t *start_lbl = lv_label_create(start);
  lv_label_set_text(start_lbl, "Start");
  lv_obj_center(start_lbl);
  lv_obj_add_event_cb(start, SetupThunks::start_evt, LV_EVENT_CLICKED, this);
  if (!cv) {
    lv_obj_set_state(start, LV_STATE_DISABLED, true);
    lv_obj_remove_flag(start, LV_OBJ_FLAG_CLICKABLE);
  } else {
    lv_obj_bind_flag_if_eq(start, &subj_link_state, LV_OBJ_FLAG_CLICKABLE, LINK_OK);
    lv_obj_bind_state_if_not_eq(start, &subj_link_state, LV_STATE_DISABLED, LINK_OK);
  }
}
