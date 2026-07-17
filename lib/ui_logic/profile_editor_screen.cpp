#include "profile_editor_screen.h"

#include <cmath>
#include <cstring>

#include "fan_resolver.h"
#include "numeric_field.h"
#include "numeric_keypad.h"
#include "panel.h"
#include "profile_curve.h"
#include "profile_facts.h"
#include "profile_templates.h"
#include "subjects.h"
#include "theme.h"
#include "value_stepper.h"

namespace {

// Round a working-copy float to the whole-unit integer the numeric editors speak (§26: every typed
// field is an integer; fractional quantities are derived). Guards a non-finite stored float.
int32_t toInt(float v) {
  if (!(v == v) || v > 3.0e9f || v < -3.0e9f) { // NaN / out of int32 range
    return 0;
  }
  return static_cast<int32_t>(v >= 0.0f ? v + 0.5f : v - 0.5f);
}

// The ambient origin the first phase ramps from (the fan/curve math needs a start temp; the
// controller measures its own). Matches profile_facts / recipe_compiler.
constexpr float kAmbientC = profile_facts::kDefaultAmbientC;

// --- Name-entry keyboard map (§12/§26) ---------------------------------------------------------
// LVGL's default keyboard uses control glyphs the body font deliberately doesn't carry — the
// newline ↵ (0xF8A2) isn't even in Font Awesome free — and its 12-column layout makes each key
// far too narrow on a 320 px panel. This is a compact map for short profile/phase names: letters,
// ⌫ backspace (0xF55A, added to the body font to match the numeric keypad) and ✓ accept
// (LV_SYMBOL_OK → LV_EVENT_READY) — every glyph present in the font. Mode switches use the literal
// "abc"/"ABC"/"1#" strings the keyboard's own handler matches. There is no cancel key: the header
// Back button is the single cancel path (back()). Dropping the cursor/newline/hide/cancel keys
// widens every remaining key.
// The maps + ctrl arrays are static: LVGL keeps the pointers, so they must outlive the keyboard.
constexpr unsigned KB_CTL = LV_KEYBOARD_CTRL_BUTTON_FLAGS; // a mode-switch / OK key
constexpr unsigned KB_BSP =
    LV_BUTTONMATRIX_CTRL_CHECKED; // backspace (styled, wide; repeats on hold)
constexpr unsigned KB_POP = LV_BUTTONMATRIX_CTRL_POPOVER;   // enlarge the key in a popover on press
constexpr unsigned KB_NRP = LV_BUTTONMATRIX_CTRL_NO_REPEAT; // one char per touch (no auto-repeat)

// A ctrl-map entry: relative width + optional flags, cast to the strongly-typed LVGL enum (LVGL's
// own C maps rely on the implicit int→enum conversion this C++ TU doesn't get).
constexpr lv_buttonmatrix_ctrl_t kbw(unsigned width, unsigned flags = 0) {
  return static_cast<lv_buttonmatrix_ctrl_t>(width | flags);
}

// A character key: width 1, a phone-style popover preview above it while pressed (the narrow keys
// are easy to fat-finger, so the popover shows which letter is under the touch), and no auto-repeat
// so holding to read the preview types exactly one character rather than a run of them.
constexpr lv_buttonmatrix_ctrl_t P = kbw(1, KB_POP | KB_NRP);

// Hand-aligned so each map row mirrors the on-screen row (and its ctrl entry lines up beneath it).
// clang-format off
const char *const kNameKbLower[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
    "ABC", "z", "x", "c", "v", "b", "n", "m", LV_SYMBOL_BACKSPACE, "\n",
    "1#", " ", LV_SYMBOL_OK, ""};
const char *const kNameKbUpper[] = {
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", "\n",
    "abc", "Z", "X", "C", "V", "B", "N", "M", LV_SYMBOL_BACKSPACE, "\n",
    "1#", " ", LV_SYMBOL_OK, ""};
// Shared 32-entry ctrl map for the two letter layouts (identical key geometry).
const lv_buttonmatrix_ctrl_t kNameKbCtrl[] = {
    P, P, P, P, P, P, P, P, P, P,                                                           // q..p
    P, P, P, P, P, P, P, P, P,                                                               // a..l
    kbw(2, KB_CTL), P, P, P, P, P, P, P, kbw(2, KB_BSP),                                     // ABC z..m ⌫
    kbw(2, KB_CTL), kbw(6), kbw(2, KB_CTL)};                                                 // 1# space ✓

const char *const kNameKbSpecial[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "-", "_", "/", ":", ";", "(", ")", "&", "@", "\n",
    "abc", ".", ",", "?", "!", "'", "+", "#", LV_SYMBOL_BACKSPACE, "\n",
    " ", LV_SYMBOL_OK, ""};
const lv_buttonmatrix_ctrl_t kNameKbSpecialCtrl[] = {
    P, P, P, P, P, P, P, P, P, P,                                                           // 1..0
    P, P, P, P, P, P, P, P, P,                                                               // - _ / : ; ( ) & @
    kbw(2, KB_CTL), P, P, P, P, P, P, P, kbw(2, KB_BSP),                                     // abc . , ? ! ' + # ⌫
    kbw(8), kbw(2, KB_CTL)};                                                                 // space ✓
// clang-format on

} // namespace

// Captureless thunks — a friend of ProfileEditorScreen so they can reach its private navigation and
// commit methods (the single-void*-user_data idiom; no std::function).
struct EditorThunks {
  static void back_evt(lv_event_t *e) {
    static_cast<ProfileEditorScreen *>(lv_event_get_user_data(e))->back();
  }
  static void save_evt(lv_event_t *e) {
    static_cast<ProfileEditorScreen *>(lv_event_get_user_data(e))->onSave();
  }
  static void phase_open(int index, void *ud) {
    static_cast<ProfileEditorScreen *>(ud)->openPhase(index);
  }
  static void field_open(int index, void *ud) {
    static_cast<ProfileEditorScreen *>(ud)->onFieldOpen(index);
  }
  static void field_commit(int32_t value, void *ud) {
    static_cast<ProfileEditorScreen *>(ud)->commitField(value);
  }
  static void field_cancel(void *ud) {
    static_cast<ProfileEditorScreen *>(ud)->showPage(ProfileEditorScreen::Page::PhaseEditor);
  }
  static void add_evt(lv_event_t *e) {
    static_cast<ProfileEditorScreen *>(lv_event_get_user_data(e))->addPhase();
  }
  static void del_evt(lv_event_t *e) {
    static_cast<ProfileEditorScreen *>(lv_event_get_user_data(e))->deletePhase();
  }
  static void mvup_evt(lv_event_t *e) {
    static_cast<ProfileEditorScreen *>(lv_event_get_user_data(e))->movePhaseUp();
  }
  static void mvdn_evt(lv_event_t *e) {
    static_cast<ProfileEditorScreen *>(lv_event_get_user_data(e))->movePhaseDown();
  }
  static void name_ok(lv_event_t *e) {
    auto *s = static_cast<ProfileEditorScreen *>(lv_event_get_user_data(e));
    s->commitName(lv_textarea_get_text(s->name_ta_));
  }
};

// --- Lifecycle ---

void ProfileEditorScreen::beginEdit(const ProfileStore::StoredProfile &working,
                                    ProfileStore &target, bool saveAs, const OvenModel &model) {
  working_ = working;
  target_ = &target;
  model_ = &model;
  save_as_ = saveAs;
  saved_ok_ = false;
  mode_ = working.mode;
  if (working_.phaseCount > kMaxPhases) {
    working_.phaseCount = kMaxPhases;
  }
  selected_phase_ = 0;
  field_sel_ = 0;
  naming_phase_ = -1;
  page_ = Page::Overview;
  recompute();
}

void ProfileEditorScreen::render(lv_obj_t *parent) {
  parent_ = parent;
  showPage(page_);
}

void ProfileEditorScreen::setExitHandler(void (*cb)(void *), void *user_data) {
  on_exit_ = cb;
  exit_ud_ = user_data;
}

// --- Validation ---

Caps ProfileEditorScreen::caps() const {
  // Editor ceilings = the mode's user max-temp setting (published cross-screen, §24); floor at
  // MIN_SEGMENT_C (0) so an accepted compile mirrors the controller's RecipeValidator (§9).
  const int32_t cap = mode_ == RecipeMode::Cure ? lv_subject_get_int(&subj_uv_cap)
                                                : lv_subject_get_int(&subj_reflow_cap);
  return Caps{0.0f, static_cast<float>(cap)};
}

void ProfileEditorScreen::recompute() {
  validation_ = compileRecipe(working_.phases, working_.phaseCount, mode_, *model_, caps(),
                              kAmbientC, /*id=*/0, /*seq=*/0);
}

const char *ProfileEditorScreen::validationWord() const {
  if (!validation_.hardValid) {
    switch (validation_.reject) {
    case CompileReject::NoPhases:
      return "NO PHASES";
    case CompileReject::NonFiniteTarget:
      return "BAD TARGET";
    case CompileReject::TargetOutOfRange:
      return "OVER LIMIT";
    case CompileReject::ModeContentMismatch:
      return "UV IN REFLOW";
    case CompileReject::TooManySegments:
      return "TOO MANY";
    case CompileReject::None:
      break;
    }
    return "INVALID";
  }
  if (validation_.hasAmber()) {
    return validation_.uncalibratedPreview ? "UNCALIBRATED" : "OPTIMISTIC";
  }
  return nullptr;
}

// --- Navigation ---

void ProfileEditorScreen::showPage(Page page) {
  clearParent();
  page_ = page;
  switch (page) {
  case Page::Overview:
    buildOverview();
    break;
  case Page::PhaseEditor:
    buildPhaseEditor();
    break;
  case Page::NameEntry:
    buildNameEntry();
    break;
  case Page::FieldEditor:
    break; // built by openField(), never showPage()
  }
}

void ProfileEditorScreen::back() {
  switch (page_) {
  case Page::Overview:
    if (on_exit_ != nullptr) {
      on_exit_(exit_ud_);
    }
    break;
  case Page::PhaseEditor:
    showPage(Page::Overview);
    break;
  case Page::FieldEditor:
    showPage(Page::PhaseEditor);
    break;
  case Page::NameEntry:
    // The header Back is the only cancel path (the keyboard has no ✗): a phase rename returns to
    // the phase editor, a profile-name (Save-as) entry to the overview.
    showPage(naming_phase_ >= 0 ? Page::PhaseEditor : Page::Overview);
    break;
  }
}

void ProfileEditorScreen::openPhase(int index) {
  if (index < 0 || static_cast<size_t>(index) >= working_.phaseCount) {
    return;
  }
  selected_phase_ = index;
  field_sel_ = 0;
  showPage(Page::PhaseEditor);
}

// --- Shared building blocks ---

void ProfileEditorScreen::clearParent() {
  lv_obj_clean(parent_);
}

void ProfileEditorScreen::configParent() {
  theme::apply_screen(parent_);
  lv_obj_set_flex_flow(parent_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(parent_, theme::PAD_S, 0);
  lv_obj_set_style_pad_row(parent_, theme::GAP, 0);
}

void ProfileEditorScreen::buildHeader(const char *title) {
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
  lv_obj_add_event_cb(back, EditorThunks::back_evt, LV_EVENT_CLICKED, this);

  lv_obj_t *title_label = lv_label_create(header);
  lv_label_set_text(title_label, title);
  lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);
  lv_obj_set_flex_grow(title_label, 1);
}

// --- Overview page ---

void ProfileEditorScreen::rebuildOverviewRows() {
  const size_t n = working_.phaseCount;
  const char *labels[kMaxPhases];
  SelectableListItem items[kMaxPhases];
  for (size_t i = 0; i < n; ++i) {
    // The phase's stored, operator-editable name (phase.h) — copied into a stable buffer so the
    // borrowed row pointer survives the page.
    std::strncpy(phase_label_[i], working_.phases[i].name, sizeof(phase_label_[i]) - 1);
    phase_label_[i][sizeof(phase_label_[i]) - 1] = '\0';
    labels[i] = phase_label_[i];
    // Compact one-row summary: target, ramp (MAX when ASAP), and hold when present (§12).
    const Phase &p = working_.phases[i];
    char ramp[12];
    if (p.rampSeconds > 0.0f) {
      std::snprintf(ramp, sizeof(ramp), "%ds", toInt(p.rampSeconds));
    } else {
      std::snprintf(ramp, sizeof(ramp), "ASAP");
    }
    const float holdVal = holdIsExposure(p) ? p.exposurePerSurface : p.holdSeconds;
    if (holdVal > 0.0f) {
      std::snprintf(phase_value_[i], sizeof(phase_value_[i]), "%d\xC2\xB0 %s \xC2\xB7 %ds",
                    toInt(p.targetC), ramp, toInt(holdVal));
    } else {
      std::snprintf(phase_value_[i], sizeof(phase_value_[i]), "%d\xC2\xB0 %s", toInt(p.targetC),
                    ramp);
    }
    items[i] = SelectableListItem{labels[i], phase_value_[i], true, "Edit"};
  }
  list_model_.init(items, static_cast<int>(n), /*wrap=*/true);
  list_model_.setOpenHandler(EditorThunks::phase_open, this);
  if (selected_phase_ >= 0 && static_cast<size_t>(selected_phase_) < n) {
    list_model_.select(selected_phase_);
  }
}

void ProfileEditorScreen::buildOverview() {
  configParent();

  // Header: "Edit: <name>" (a named profile) or "New profile" (a fresh, unnamed one).
  if (working_.name[0] != '\0') {
    std::snprintf(header_buf_, sizeof(header_buf_), "Edit: %s", working_.name);
  } else {
    std::snprintf(header_buf_, sizeof(header_buf_), "New profile");
  }
  buildHeader(header_buf_);

  // Feasibility curve — requested (dashed ghost) vs settling (solid predicted actual), derived from
  // the working copy.
  profile_facts::CurvePoint req[profile_facts::kMaxCurvePoints];
  const size_t nr = profile_facts::sampleCurve(working_.phases, working_.phaseCount, mode_, *model_,
                                               /*achievable=*/false, kAmbientC, req,
                                               profile_facts::kMaxCurvePoints);
  profile_facts::CurvePoint over[profile_facts::kMaxCurvePoints];
  const size_t no =
      profile_facts::sampleOvershoot(working_.phases, working_.phaseCount, mode_, *model_,
                                     kAmbientC, over, profile_facts::kMaxCurvePoints);
  const bool rate_limited = profile_facts::anyRampRateLimited(working_.phases, working_.phaseCount,
                                                              mode_, *model_, kAmbientC);
  float bounds[profile_facts::kMaxCurvePhases];
  const size_t nb =
      profile_facts::samplePhaseBoundaries(working_.phases, working_.phaseCount, mode_, *model_,
                                           kAmbientC, bounds, profile_facts::kMaxCurvePhases);
  profile_facts::TimeSpan uv[kMaxPhases];
  const size_t nuv = profile_facts::sampleUvSpans(working_.phases, working_.phaseCount, mode_,
                                                  *model_, kAmbientC, uv, kMaxPhases);
  // Phase labels parallel to the boundaries: each phase's stored name, then "Cool" for the implicit
  // passive cool-down the samplers append when the run ends hot (implicit_cool.h, §6).
  char namebuf[profile_facts::kMaxCurvePhases][kPhaseNameCap];
  const char *names[profile_facts::kMaxCurvePhases];
  const size_t authored = working_.phaseCount > kMaxPhases ? kMaxPhases : working_.phaseCount;
  for (size_t i = 0; i < authored; ++i) {
    std::strncpy(namebuf[i], working_.phases[i].name, sizeof(namebuf[i]) - 1);
    namebuf[i][sizeof(namebuf[i]) - 1] = '\0';
    names[i] = namebuf[i];
  }
  if (nb > authored) {
    names[authored] = kImplicitCoolLabel;
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
  cd.n_phase_names = nb;
  cd.uncalibrated = false; // the amber banner below already says UNCALIBRATED — don't say it twice
  cd.rate_limited = rate_limited;
  create_profile_curve(parent_, cd);

  // Validation banner — red word (blocks Save) when hard-invalid, amber word (allows) when
  // optimistic; nothing when clean (§12).
  const char *word = validationWord();
  if (word != nullptr) {
    const bool block = !validation_.hardValid;
    lv_obj_t *banner = lv_obj_create(parent_);
    theme::apply_alert(banner, block ? theme::FAULT : theme::WARN);
    lv_obj_set_width(banner, lv_pct(100));
    lv_obj_set_height(banner, theme::BANNER_H);
    lv_obj_t *lbl = lv_label_create(banner);
    lv_label_set_text(lbl, word);
    lv_obj_center(lbl);
  }

  // Advanced structure controls (§12) — only when unlocked; act on the highlighted phase.
  if (lv_subject_get_int(&subj_advanced) != 0) {
    lv_obj_t *bar = lv_obj_create(parent_);
    theme::apply_row(bar);
    lv_obj_set_width(bar, lv_pct(100));
    lv_obj_set_height(bar, theme::SECONDARY_H);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(bar, theme::GAP, 0);
    struct AdvBtn {
      const char *text;
      lv_event_cb_t cb;
    };
    const AdvBtn btns[4] = {{"+ Add", EditorThunks::add_evt},
                            {"Del", EditorThunks::del_evt},
                            {"Up", EditorThunks::mvup_evt},
                            {"Dn", EditorThunks::mvdn_evt}};
    for (const AdvBtn &b : btns) {
      lv_obj_t *btn = lv_button_create(bar);
      theme::apply_secondary(btn);
      lv_obj_set_flex_grow(btn, 1);
      lv_obj_set_height(btn, lv_pct(100));
      lv_obj_t *l = lv_label_create(btn);
      lv_label_set_text(l, b.text);
      lv_obj_center(l);
      lv_obj_add_event_cb(btn, b.cb, LV_EVENT_CLICKED, this);
    }
  }

  // Phase list + footer. The leading footer button is Save (commits the whole profile); Edit drills
  // into the highlighted phase's field editor. Save disables while the profile is hard-invalid.
  rebuildOverviewRows();
  LeadingAction save{"Save", EditorThunks::save_evt, this};
  SelectableList list = create_selectable_list(parent_, list_model_, save);
  if (list.btn_leading != nullptr && !validation_.hardValid) {
    lv_obj_add_state(list.btn_leading, LV_STATE_DISABLED);
  }
  // Keep the highlighted phase in view across rebuilds (add/delete/reorder/return-from-editor): the
  // selection observer scrolls on select, but the fresh list is not laid out yet, so re-scroll
  // after forcing layout — otherwise the list snaps to the top.
  if (selected_phase_ >= 0 && static_cast<size_t>(selected_phase_) < working_.phaseCount) {
    lv_obj_update_layout(parent_);
    lv_obj_scroll_to_view(list.rows[selected_phase_], LV_ANIM_OFF);
  }
}

// --- Phase editor page ---

bool ProfileEditorScreen::holdIsExposure(const Phase &p) const {
  // Dose authoring only when cure + calibrated + turntable + UV (else it is raw seconds, §5/§12).
  return mode_ == RecipeMode::Cure && model_->calibrated && p.uv && p.motor &&
         model_->beamCoverage > 0.0f;
}

const char *ProfileEditorScreen::holdRowLabel(const Phase &p) const {
  return holdIsExposure(p) ? "UV / surface" : "Hold (s)";
}

const char *ProfileEditorScreen::holdNote(const Phase &p) const {
  if (mode_ != RecipeMode::Cure) {
    return nullptr;
  }
  // Turntable off with UV on: only the facing surface is dosed, and the value is raw seconds (§12).
  if (p.uv && !p.motor) {
    return "only the facing surface is exposed";
  }
  if (!model_->calibrated) {
    return "uncalibrated - raw seconds";
  }
  if (holdIsExposure(p)) {
    return "estimated"; // dose from the conservative default beamCoverage
  }
  return nullptr;
}

void ProfileEditorScreen::buildPhaseEditor() {
  configParent();
  if (working_.phaseCount == 0) {
    showPage(Page::Overview);
    return;
  }
  if (static_cast<size_t>(selected_phase_) >= working_.phaseCount) {
    selected_phase_ = static_cast<int>(working_.phaseCount) - 1;
  }
  const Phase &p = currentPhase();

  // Header: which phase, "<Name> (i/N)" — the phase's stored, editable name.
  std::snprintf(header_buf_, sizeof(header_buf_), "%s (%d/%u)", p.name, selected_phase_ + 1,
                static_cast<unsigned>(working_.phaseCount));
  buildHeader(header_buf_);

  // A per-phase amber note (cure hold semantics, §12), when one applies.
  if (const char *note = holdNote(p)) {
    lv_obj_t *cap = lv_label_create(parent_);
    lv_label_set_text(cap, note);
    lv_obj_set_style_text_color(cap, theme::col(theme::WARN), 0);
    lv_obj_set_width(cap, lv_pct(100));
    lv_label_set_long_mode(cap, LV_LABEL_LONG_WRAP);
  }

  // Build the field rows (a row→action map, since cure adds UV/motor — the SettingsScreen idiom).
  std::strncpy(hold_label_, holdRowLabel(p), sizeof(hold_label_) - 1);
  hold_label_[sizeof(hold_label_) - 1] = '\0';

  SelectableListItem items[7];
  int n = 0;
  auto add = [&](FieldRow row, const char *label, const char *value, const char *verb) {
    field_rows_[n] = row;
    items[n] = SelectableListItem{label, value, true, verb};
    ++n;
  };

  // Name — free-text, opens the keyboard (value borrows p.name, stable in the working copy).
  add(FieldRow::Name, "Name", p.name, "Rename");
  // Target.
  std::snprintf(field_value_[0], sizeof(field_value_[0]),
                "%d \xC2\xB0"
                "C",
                toInt(p.targetC));
  add(FieldRow::Target, "Target temp", field_value_[0], "Edit");
  // Ramp (min = MAX / ASAP).
  if (p.rampSeconds > 0.0f) {
    std::snprintf(field_value_[1], sizeof(field_value_[1]), "%d s", toInt(p.rampSeconds));
  } else {
    std::snprintf(field_value_[1], sizeof(field_value_[1]), "ASAP");
  }
  add(FieldRow::Ramp, "Ramp time", field_value_[1], "Edit");
  // Hold (label + units track the cure/reflow semantics).
  const float holdVal = holdIsExposure(p) ? p.exposurePerSurface : p.holdSeconds;
  if (holdIsExposure(p)) {
    std::snprintf(field_value_[2], sizeof(field_value_[2]), "%d", toInt(holdVal));
  } else {
    std::snprintf(field_value_[2], sizeof(field_value_[2]), "%d s", toInt(holdVal));
  }
  add(FieldRow::Hold, hold_label_, field_value_[2], "Edit");

  // Fans (tri-state; on Auto show the resolved state in parentheses).
  const float fromC =
      selected_phase_ > 0 ? working_.phases[selected_phase_ - 1].targetC : kAmbientC;
  const FanContext fc{fromC, p.targetC, p.rampSeconds};
  const FanDecision fans = resolveFans(p.convFan, fc, *model_);
  auto fanText = [](FanMode m, bool resolved, char *buf, size_t cap) {
    switch (m) {
    case FanMode::On:
      std::snprintf(buf, cap, "On");
      break;
    case FanMode::Off:
      std::snprintf(buf, cap, "Off");
      break;
    case FanMode::Auto:
      std::snprintf(buf, cap, "Auto (%s)", resolved ? "on" : "off");
      break;
    }
  };
  fanText(p.convFan, fans.convFan, field_value_[3], sizeof(field_value_[3]));
  add(FieldRow::ConvFan, "Conv fan", field_value_[3], "Change");

  // UV / motor — cure mode only (reflow phases carry no such channels, §4).
  if (mode_ == RecipeMode::Cure) {
    std::snprintf(field_value_[4], sizeof(field_value_[4]), "%s", p.uv ? "On" : "Off");
    add(FieldRow::Uv, "UV", field_value_[4], "Toggle");
    std::snprintf(field_value_[5], sizeof(field_value_[5]), "%s", p.motor ? "On" : "Off");
    add(FieldRow::Motor, "Turntable", field_value_[5], "Toggle");
  }

  field_row_count_ = n;
  list_model_.init(items, n, /*wrap=*/true);
  list_model_.setOpenHandler(EditorThunks::field_open, this);
  SelectableList list = create_selectable_list(parent_, list_model_);
  // Keep the highlighted field in view across an in-place toggle/edit rebuild: select fires the
  // scroll observer, but the fresh list is not laid out yet, so re-scroll after forcing layout —
  // otherwise toggling a row (e.g. a fan) snaps the list back to the top.
  if (field_sel_ >= 0 && field_sel_ < n) {
    list_model_.select(field_sel_);
    lv_obj_update_layout(parent_);
    lv_obj_scroll_to_view(list.rows[field_sel_], LV_ANIM_OFF);
  }
}

void ProfileEditorScreen::onFieldOpen(int index) {
  if (index < 0 || index >= field_row_count_) {
    return;
  }
  field_sel_ = index;
  Phase &p = currentPhase();
  switch (field_rows_[index]) {
  case FieldRow::Name:
    openPhaseName(selected_phase_);
    break;
  case FieldRow::Target:
    openField(NumField::Target);
    break;
  case FieldRow::Ramp:
    openField(NumField::Ramp);
    break;
  case FieldRow::Hold:
    openField(NumField::Hold);
    break;
  case FieldRow::ConvFan:
    p.convFan = cycleFan(p.convFan);
    recompute();
    showPage(Page::PhaseEditor);
    break;
  case FieldRow::Uv:
    p.uv = !p.uv; // flips hold semantics + validation (§12) — recompute + rebuild
    recompute();
    showPage(Page::PhaseEditor);
    break;
  case FieldRow::Motor:
    p.motor = !p.motor;
    recompute();
    showPage(Page::PhaseEditor);
    break;
  }
}

FanMode ProfileEditorScreen::cycleFan(FanMode m) {
  switch (m) {
  case FanMode::Auto:
    return FanMode::On;
  case FanMode::On:
    return FanMode::Off;
  case FanMode::Off:
    return FanMode::Auto;
  }
  return FanMode::Auto;
}

// --- Numeric field editor ---

NumericFieldConfig ProfileEditorScreen::fieldConfig(NumField field) const {
  const int32_t cap = mode_ == RecipeMode::Cure ? lv_subject_get_int(&subj_uv_cap)
                                                : lv_subject_get_int(&subj_reflow_cap);
  switch (field) {
  case NumField::Target:
    // Ceiling = the mode cap (§12); floor 0. Wide range → the keypad (>20-step rule).
    return NumericFieldConfig{0,
                              cap,
                              1,
                              0,
                              "\xC2\xB0"
                              "C",
                              nullptr};
  case NumField::Ramp:
    // 0 = MAX/ASAP; up to an hour.
    return NumericFieldConfig{0, 3600, 1, 0, "s", nullptr};
  case NumField::Hold:
    return holdIsExposure(currentPhase()) ? NumericFieldConfig{0, 10000, 1, 0, "", nullptr}
                                          : NumericFieldConfig{0, 7200, 1, 0, "s", nullptr};
  case NumField::None:
    break;
  }
  return NumericFieldConfig{};
}

void ProfileEditorScreen::openField(NumField field) {
  const Phase &p = currentPhase();
  int32_t initial = 0;
  const char *title = "";
  switch (field) {
  case NumField::Target:
    initial = toInt(p.targetC);
    title = "Target temp";
    break;
  case NumField::Ramp:
    initial = toInt(p.rampSeconds);
    title = "Ramp time (0 = ASAP)";
    break;
  case NumField::Hold:
    initial = toInt(holdIsExposure(p) ? p.exposurePerSurface : p.holdSeconds);
    title = holdIsExposure(p) ? "UV / surface" : "Hold (s)";
    break;
  case NumField::None:
    return;
  }

  const NumericFieldConfig cfg = fieldConfig(field);
  clearParent();
  editing_field_ = field;
  page_ = Page::FieldEditor;

  // The >20-step rule picks the editor; phase temps/times are all wide, so this is the keypad in
  // practice — but route through usesStepper() so the choice stays data-driven (§24).
  if (cfg.usesStepper()) {
    stepper_vm_.init(cfg, initial);
    stepper_vm_.setCommitHandler(EditorThunks::field_commit, this);
    stepper_vm_.setCancelHandler(EditorThunks::field_cancel, this);
    create_value_stepper(parent_, stepper_vm_, title);
  } else {
    keypad_vm_.init(cfg, initial);
    keypad_vm_.setCommitHandler(EditorThunks::field_commit, this);
    keypad_vm_.setCancelHandler(EditorThunks::field_cancel, this);
    create_numeric_keypad(parent_, keypad_vm_, title);
  }
}

void ProfileEditorScreen::commitField(int32_t value) {
  Phase &p = currentPhase();
  switch (editing_field_) {
  case NumField::Target:
    p.targetC = static_cast<float>(value);
    break;
  case NumField::Ramp:
    p.rampSeconds = static_cast<float>(value);
    break;
  case NumField::Hold:
    if (holdIsExposure(p)) {
      p.exposurePerSurface = static_cast<float>(value);
    } else {
      p.holdSeconds = static_cast<float>(value);
    }
    break;
  case NumField::None:
    break;
  }
  editing_field_ = NumField::None;
  recompute();
  showPage(Page::PhaseEditor);
}

// --- Advanced structure edits ---

void ProfileEditorScreen::addPhase() {
  if (working_.phaseCount >= kMaxPhases) {
    return;
  }
  const size_t at = static_cast<size_t>(selected_phase_) + 1; // insert after the highlighted phase
  for (size_t i = working_.phaseCount; i > at; --i) {
    working_.phases[i] = working_.phases[i - 1];
  }
  working_.phases[at] = profile_templates::blankPhase();
  ++working_.phaseCount;
  // A new phase is off the canonical template, so it seeds a generic "Phase N" name; the operator
  // renames it from the phase editor. Never leave it nameless (phases carry an explicit name).
  profile_templates::seedPhaseName(mode_, at, working_.phaseCount, working_.phases[at]);
  selected_phase_ = static_cast<int>(at);
  recompute();
  if (page_ == Page::Overview) {
    showPage(Page::Overview);
  }
}

void ProfileEditorScreen::deletePhase() {
  if (working_.phaseCount <= 1) {
    return; // never leave a profile with no phases (would be hard-invalid + unrecoverable in-place)
  }
  const size_t at = static_cast<size_t>(selected_phase_);
  for (size_t i = at; i + 1 < working_.phaseCount; ++i) {
    working_.phases[i] = working_.phases[i + 1];
  }
  --working_.phaseCount;
  if (static_cast<size_t>(selected_phase_) >= working_.phaseCount) {
    selected_phase_ = static_cast<int>(working_.phaseCount) - 1;
  }
  recompute();
  if (page_ == Page::Overview) {
    showPage(Page::Overview);
  }
}

void ProfileEditorScreen::movePhaseUp() {
  if (selected_phase_ <= 0 || static_cast<size_t>(selected_phase_) >= working_.phaseCount) {
    return;
  }
  const size_t i = static_cast<size_t>(selected_phase_);
  Phase tmp = working_.phases[i - 1];
  working_.phases[i - 1] = working_.phases[i];
  working_.phases[i] = tmp;
  --selected_phase_;
  recompute();
  if (page_ == Page::Overview) {
    showPage(Page::Overview);
  }
}

void ProfileEditorScreen::movePhaseDown() {
  if (selected_phase_ < 0 || static_cast<size_t>(selected_phase_) + 1 >= working_.phaseCount) {
    return;
  }
  const size_t i = static_cast<size_t>(selected_phase_);
  Phase tmp = working_.phases[i + 1];
  working_.phases[i + 1] = working_.phases[i];
  working_.phases[i] = tmp;
  ++selected_phase_;
  recompute();
  if (page_ == Page::Overview) {
    showPage(Page::Overview);
  }
}

// --- Save + name entry ---

void ProfileEditorScreen::onSave() {
  recompute();
  if (!validation_.hardValid) {
    return; // Save is disabled in the UI; guard the seam too
  }
  if (save_as_ || working_.name[0] == '\0') {
    naming_phase_ = -1; // the profile name (Save-as), not a phase rename
    showPage(Page::NameEntry);
    return;
  }
  doSave();
}

void ProfileEditorScreen::openPhaseName(int index) {
  if (index < 0 || static_cast<size_t>(index) >= working_.phaseCount) {
    return;
  }
  selected_phase_ = index;
  naming_phase_ = index; // the keyboard now targets this phase's name
  showPage(Page::NameEntry);
}

void ProfileEditorScreen::doSave() {
  saved_ok_ = target_->save(working_);
  if (saved_ok_) {
    if (on_exit_ != nullptr) {
      on_exit_(exit_ud_);
    }
  } else {
    showPage(Page::Overview); // a rejected save (e.g. name taken by a stock profile) → back to edit
  }
}

void ProfileEditorScreen::commitName(const char *name) {
  // Phase rename: lighter validation (a phase name is not a filesystem key), write into the phase,
  // and return to the phase editor. No Save — a rename is just another working-copy edit.
  if (naming_phase_ >= 0) {
    if (static_cast<size_t>(naming_phase_) >= working_.phaseCount ||
        !ProfileStore::validPhaseName(name)) {
      return; // stay on the keyboard until a valid name is given
    }
    Phase &p = working_.phases[naming_phase_];
    std::strncpy(p.name, name, kPhaseNameCap - 1);
    p.name[kPhaseNameCap - 1] = '\0';
    naming_phase_ = -1;
    showPage(Page::PhaseEditor);
    return;
  }
  // Profile name (Save-as, §23): filesystem-key validation, then commit the whole profile.
  if (!ProfileStore::validName(name)) {
    return; // stay on name entry until a valid name is given
  }
  std::strncpy(working_.name, name, kProfileNameCap - 1);
  working_.name[kProfileNameCap - 1] = '\0';
  save_as_ = false;
  doSave();
}

void ProfileEditorScreen::buildNameEntry() {
  configParent();
  const bool phase = naming_phase_ >= 0 && static_cast<size_t>(naming_phase_) < working_.phaseCount;
  buildHeader(phase ? "Rename phase" : "Name");

  // The buffer being edited + its cap depend on the target (a phase name is shorter than a profile
  // name, so clamp the textarea to the tighter limit).
  const char *cur = phase ? working_.phases[naming_phase_].name : working_.name;
  const uint32_t cap = static_cast<uint32_t>((phase ? kPhaseNameCap : kProfileNameCap) - 1);

  name_ta_ = lv_textarea_create(parent_);
  lv_textarea_set_one_line(name_ta_, true);
  lv_textarea_set_max_length(name_ta_, cap);
  lv_textarea_set_placeholder_text(name_ta_, phase ? "Phase name" : "Profile name");
  if (cur[0] != '\0') {
    lv_textarea_set_text(name_ta_, cur);
  }
  lv_obj_set_width(name_ta_, lv_pct(100));

  // A flex-grow spacer pins the keyboard to the bottom while the field sits under the header.
  lv_obj_t *spacer = lv_obj_create(parent_);
  lv_obj_remove_style_all(spacer);
  lv_obj_set_width(spacer, lv_pct(100));
  lv_obj_set_flex_grow(spacer, 1);
  lv_obj_remove_flag(spacer, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t *kb = lv_keyboard_create(parent_);
  lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_LOWER, kNameKbLower, kNameKbCtrl);
  lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_UPPER, kNameKbUpper, kNameKbCtrl);
  lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_SPECIAL, kNameKbSpecial, kNameKbSpecialCtrl);
  lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_keyboard_set_popovers(kb, true); // honor the POPOVER ctrl flags — enlarge each pressed key
  lv_keyboard_set_textarea(kb, name_ta_);
  // Bound the height so 4 rows read ~square, not tall-and-narrow (mm-authored, capped so it still
  // fits the short landscape panel). flex_grow would stretch it over the whole lower screen.
  int32_t kb_h = panel::pxFromMmX10(360); // ~36 mm of keys
  const int32_t kb_cap = (panel::H * 55) / 100;
  if (kb_h > kb_cap) {
    kb_h = kb_cap;
  }
  lv_obj_set_width(kb, lv_pct(100));
  lv_obj_set_height(kb, kb_h);
  lv_obj_add_event_cb(kb, EditorThunks::name_ok, LV_EVENT_READY, this);
}

// --- Accessors ---

Phase &ProfileEditorScreen::currentPhase() {
  size_t i = static_cast<size_t>(selected_phase_);
  if (i >= working_.phaseCount) {
    i = working_.phaseCount > 0 ? working_.phaseCount - 1 : 0;
  }
  return working_.phases[i];
}

const Phase &ProfileEditorScreen::currentPhase() const {
  size_t i = static_cast<size_t>(selected_phase_);
  if (i >= working_.phaseCount) {
    i = working_.phaseCount > 0 ? working_.phaseCount - 1 : 0;
  }
  return working_.phases[i];
}
