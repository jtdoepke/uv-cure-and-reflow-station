#include "profile_library_screen.h"

#include <initializer_list>

#include "confirm_dialog.h"
#include "link_banner.h"   // shared "Controller not responding" banner (§9/§14)
#include "name_keyboard.h" // the shared name-entry keyboard (profile Rename)
#include "profile_curve.h"
#include "subjects.h"
#include "theme.h"

// The library lists up to kMaxRows; the whole list binds to one SelectableListModel so ▲/▼ can
// walk it, so the model must hold at least as many. (The header keeps this dependency out of the
// generic widget; the assert lives here where both types are known.)
static_assert(ProfileLibraryViewModel::kMaxRows <=
                  static_cast<size_t>(SelectableListModel::kMaxItems),
              "SelectableListModel::kMaxItems must cover a full profile library");

// Captureless thunks — a friend of ProfileLibraryScreen so they can reach its private navigation /
// action methods (the codebase's single-void*-user_data idiom, no std::function).
struct ProfileThunks {
  static void choose_cure(lv_event_t *e) {
    static_cast<ProfileLibraryScreen *>(lv_event_get_user_data(e))->openMode(RecipeMode::Cure);
  }
  static void choose_reflow(lv_event_t *e) {
    static_cast<ProfileLibraryScreen *>(lv_event_get_user_data(e))->openMode(RecipeMode::Reflow);
  }
  static void list_open(int index, void *ud) {
    static_cast<ProfileLibraryScreen *>(ud)->openDetail(index);
  }
  static void back_evt(lv_event_t *e) {
    static_cast<ProfileLibraryScreen *>(lv_event_get_user_data(e))->back();
  }
  static void new_evt(lv_event_t *e) {
    static_cast<ProfileLibraryScreen *>(lv_event_get_user_data(e))->onNew();
  }
  static void edit_evt(lv_event_t *e) {
    static_cast<ProfileLibraryScreen *>(lv_event_get_user_data(e))->onEdit();
  }
  static void dup_evt(lv_event_t *e) {
    static_cast<ProfileLibraryScreen *>(lv_event_get_user_data(e))->onDuplicate();
  }
  static void rename_evt(lv_event_t *e) {
    static_cast<ProfileLibraryScreen *>(lv_event_get_user_data(e))->onRenameRequested();
  }
  static void rename_ok(lv_event_t *e) {
    auto *s = static_cast<ProfileLibraryScreen *>(lv_event_get_user_data(e));
    s->onRenameCommit(lv_textarea_get_text(s->rename_ta_));
  }
  static void delete_evt(lv_event_t *e) {
    static_cast<ProfileLibraryScreen *>(lv_event_get_user_data(e))->onDeleteRequested();
  }
  static void confirm_delete(void *ud) {
    static_cast<ProfileLibraryScreen *>(ud)->onDeleteConfirmed();
  }
  static void cancel_delete(void *ud) { static_cast<ProfileLibraryScreen *>(ud)->back(); }
};

// --- Lifecycle ---

void ProfileLibraryScreen::begin(lv_obj_t *parent, ManagementClient &client,
                                 const OvenModel &model) {
  parent_ = parent;
  client_ = &client;
  model_ = &model;
  showChooser(); // the one view-model is init'd per mode in openMode()
}

void ProfileLibraryScreen::setExitHandler(void (*cb)(void *), void *user_data) {
  on_exit_ = cb;
  exit_ud_ = user_data;
}

void ProfileLibraryScreen::publishNav(int nav_request) {
  lv_subject_set_int(&subj_nav_request, nav_request);
}

// --- Shared building blocks ---

void ProfileLibraryScreen::clearParent() {
  lv_obj_clean(parent_);
}

void ProfileLibraryScreen::configParent() {
  theme::apply_screen(parent_);
  lv_obj_set_flex_flow(parent_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(parent_, theme::PAD_S, 0);
  lv_obj_set_style_pad_row(parent_, theme::GAP, 0);
}

void ProfileLibraryScreen::buildHeader(const char *title) {
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
  lv_obj_add_event_cb(back, ProfileThunks::back_evt, LV_EVENT_CLICKED, this);

  lv_obj_t *title_label = lv_label_create(header);
  lv_label_set_text(title_label, title);
  lv_obj_set_flex_grow(title_label, 1);
  lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);

  // The library lists/opens profiles that live on the controller now (§7/§9), so surface a dropped
  // link here as everywhere. buildDetail() builds its own header and adds the banner itself.
  create_link_banner(parent_);
}

// --- Navigation ---

void ProfileLibraryScreen::showChooser() {
  pending_ = Pending::None; // dropping any in-flight interest; a new op overwrites the client
  page_ = Page::Chooser;
  buildChooser();
}

void ProfileLibraryScreen::openMode(RecipeMode mode) {
  mode_ = mode;
  current_ = &vm_;
  vm_.init(*client_, phase_codec::modeToWire(mode), *model_);
  current_->setFahrenheit(lv_subject_get_int(&subj_units) != 0);
  selected_ = 0;
  return_page_ = Page::Chooser;
  // Fetch this mode's library from the controller (§9). The reply lands in poll() → buildList().
  if (current_->requestList()) {
    page_ = Page::Loading;
    pending_ = Pending::List;
    buildLoading("Loading profiles...");
  } else {
    page_ = Page::Error;
    buildError();
  }
}

void ProfileLibraryScreen::openDetail(int index) {
  selected_ = index;
  return_page_ = Page::List;
  // Fetch the full profile for the curve (§9). The reply lands in poll() → buildDetail().
  if (current_->requestDetail(static_cast<size_t>(index))) {
    page_ = Page::Loading;
    pending_ = Pending::Detail;
    buildLoading("Loading...");
  } else {
    page_ = Page::Error;
    buildError();
  }
}

// Drive the async state machine (called every loop after client.service()).
void ProfileLibraryScreen::poll() {
  if (pending_ == Pending::None || client_ == nullptr || client_->busy()) {
    return;
  }
  if (client_->failed()) {
    pending_ = Pending::None;
    client_->clear();
    page_ = Page::Error;
    buildError();
    return;
  }
  // Ready — dispatch on what we asked for.
  switch (pending_) {
  case Pending::List:
    current_->adoptList(client_->list());
    client_->clear();
    pending_ = Pending::None;
    page_ = Page::List;
    buildList();
    break;
  case Pending::Detail:
    current_->adoptDetail(client_->profile());
    client_->clear();
    pending_ = Pending::None;
    page_ = Page::Detail;
    buildDetail();
    break;
  case Pending::Action:
    // A mutation (dup/rename/delete) succeeded — re-list to reflect it.
    client_->clear();
    if (current_->requestList()) {
      pending_ = Pending::List; // stay on Loading until the fresh list arrives
    } else {
      pending_ = Pending::None;
      page_ = Page::List;
      buildList();
    }
    break;
  case Pending::None:
    break;
  }
}

void ProfileLibraryScreen::buildLoading(const char *msg) {
  clearParent();
  configParent();
  buildHeader(mode_ == RecipeMode::Cure ? "Cure profiles" : "Reflow profiles");
  lv_obj_t *lbl = lv_label_create(parent_);
  lv_label_set_text(lbl, msg);
  lv_obj_set_flex_grow(lbl, 1);
  lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
  theme::apply_caption(lbl);
}

void ProfileLibraryScreen::buildError() {
  clearParent();
  configParent();
  buildHeader("Profiles");
  lv_obj_t *lbl = lv_label_create(parent_);
  // The one thing a UI remote must say when the link is down (§22 wording is for run faults; this
  // is a benign idle-context management failure, so a plain caption, not the red modal).
  lv_label_set_text(lbl, "Couldn't reach the controller.\nCheck the link and try again.");
  lv_obj_set_width(lbl, lv_pct(100));
  lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_flex_grow(lbl, 1);
  lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
  theme::apply_caption(lbl);
}

void ProfileLibraryScreen::back() {
  switch (page_) {
  case Page::ConfirmDelete:
    page_ = Page::Detail;
    buildDetail(); // rebuild detail (clears the overlay dialog with it)
    break;
  case Page::Rename:
    page_ = Page::Detail;
    buildDetail(); // cancel the rename — back to the profile detail
    break;
  case Page::Detail:
    page_ = Page::List;
    buildList();
    break;
  case Page::List:
    showChooser();
    break;
  case Page::Loading:
  case Page::Error:
    // Abandon the in-flight/failed request and return to the last stable page. A late reply is
    // ignored (pending_ cleared) and a subsequent request just overwrites the client.
    pending_ = Pending::None;
    if (return_page_ == Page::List) {
      page_ = Page::List;
      buildList();
    } else {
      showChooser();
    }
    break;
  case Page::Chooser:
    if (on_exit_ != nullptr) {
      on_exit_(exit_ud_);
    }
    break;
  }
}

// --- Chooser (Cure | Reflow) ---

namespace {
// A big Home-style mode tile (§14 apply_mode_tile), `screen` as the click user_data.
lv_obj_t *make_mode_tile(lv_obj_t *parent, const char *text, lv_event_cb_t on_click, void *screen) {
  lv_obj_t *btn = lv_button_create(parent);
  theme::apply_mode_tile(btn);
  lv_obj_t *label = lv_label_create(btn);
  lv_label_set_text(label, text);
  // The two-word labels ("UV CURE PROFILES") can exceed a tile's width on the narrow 2.8" side-by-
  // side layout, so wrap + centre rather than clip.
  lv_obj_set_width(label, lv_pct(90));
  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(label);
  lv_obj_add_event_cb(btn, on_click, LV_EVENT_CLICKED, screen);
  return btn;
}
} // namespace

void ProfileLibraryScreen::buildChooser() {
  clearParent();
  configParent();
  buildHeader("Profiles");

  // Exactly two profile types, so this is two big Home-style tiles (a direct tap), not a ▲/▼ list.
  // Each tile fetches that mode's library from the controller (§7/§9 — the profiles are no longer
  // CYD-local), so both are link-gated: greyed + non-clickable when the link is down, like Home's
  // run tiles, with the banner above saying why. (Gated below, after the tiles are built.)
  lv_obj_t *modes = lv_obj_create(parent_);
  theme::apply_row(modes);
  lv_obj_set_width(modes, lv_pct(100));
  lv_obj_set_flex_grow(modes, 1);
  lv_obj_set_flex_flow(modes, panel::kPortrait ? LV_FLEX_FLOW_COLUMN : LV_FLEX_FLOW_ROW);

  lv_obj_t *cure = make_mode_tile(modes, "UV CURE PROFILES", ProfileThunks::choose_cure, this);
  lv_obj_t *reflow = make_mode_tile(modes, "REFLOW PROFILES", ProfileThunks::choose_reflow, this);
  for (lv_obj_t *tile : {cure, reflow}) {
    lv_obj_set_flex_grow(tile, 1);
    if (panel::kPortrait) {
      lv_obj_set_width(tile, lv_pct(100));
    } else {
      lv_obj_set_height(tile, lv_pct(100));
    }
    lv_obj_bind_flag_if_eq(tile, &subj_link_state, LV_OBJ_FLAG_CLICKABLE, LINK_OK);
    lv_obj_bind_state_if_not_eq(tile, &subj_link_state, LV_STATE_DISABLED, LINK_OK);
  }
}

// --- Mode-scoped library list ---

void ProfileLibraryScreen::buildList() {
  clearParent();
  configParent();
  buildHeader(mode_ == RecipeMode::Cure ? "Cure profiles" : "Reflow profiles");

  const size_t n = current_->count();
  SelectableListItem items[ProfileLibraryViewModel::kMaxRows];
  for (size_t i = 0; i < n; ++i) {
    // Borrowed pointers into the VM's buffers (they outlive this list — VM is a member).
    items[i] = SelectableListItem{current_->rowLabel(i), current_->rowValue(i), true, "Open"};
  }
  list_model_.init(items, static_cast<int>(n), /*wrap=*/false);
  list_model_.setOpenHandler(ProfileThunks::list_open, this);

  const LeadingAction new_action{"+ New", ProfileThunks::new_evt, this};
  SelectableList ui = create_selectable_list(parent_, list_model_, new_action);

  if (n == 0) {
    // §23 empty state. Shouldn't happen once stock seeds ship, but a fresh flash has no profiles.
    lv_obj_t *empty = lv_label_create(ui.list);
    lv_label_set_text(empty, "No profiles - New to create one"); // ASCII dash (font has no em dash)
    lv_obj_set_width(empty, lv_pct(100));
    lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
    theme::apply_caption(empty);
  } else {
    list_model_.select(selected_ < static_cast<int>(n) ? selected_ : 0);
  }
}

// --- Profile detail / actions ---

namespace {
// One action button in the detail footer. `enabled == false` renders it greyed + non-clickable
// (stock Delete). `this_screen` is the click user_data.
lv_obj_t *action_button(lv_obj_t *row, const char *text, lv_event_cb_t cb, void *this_screen,
                        bool enabled) {
  lv_obj_t *btn = lv_button_create(row);
  theme::apply_secondary(btn);
  lv_obj_set_flex_grow(btn, 1);
  lv_obj_set_height(btn, lv_pct(100));
  lv_obj_t *label = lv_label_create(btn);
  lv_label_set_text(label, text);
  lv_obj_center(label);
  if (enabled) {
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, this_screen);
    // Every detail action (Delete/Rename/Clone/Edit) issues a management request or opens the async
    // editor (§9), so gate it on a healthy link — greyed + non-clickable when down, matching Home's
    // run-tile gate; the banner above says why. Re-enables reactively on reconnect.
    lv_obj_bind_flag_if_eq(btn, &subj_link_state, LV_OBJ_FLAG_CLICKABLE, LINK_OK);
    lv_obj_bind_state_if_not_eq(btn, &subj_link_state, LV_STATE_DISABLED, LINK_OK);
  } else {
    lv_obj_set_state(btn, LV_STATE_DISABLED, true);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_CLICKABLE);
  }
  return btn;
}
} // namespace

void ProfileLibraryScreen::buildDetail() {
  clearParent();
  configParent();

  // Header: Back + profile name + mode word (word, never colour alone — §14).
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
  lv_obj_add_event_cb(back, ProfileThunks::back_evt, LV_EVENT_CLICKED, this);

  lv_obj_t *name = lv_label_create(header);
  lv_label_set_text(name, current_->name(selected_));
  lv_obj_set_flex_grow(name, 1);
  lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);

  lv_obj_t *badge = lv_label_create(header);
  lv_label_set_text(badge, mode_ == RecipeMode::Cure ? "Cure" : "Reflow");
  lv_obj_set_style_text_color(badge, theme::col(theme::ACCENT), 0);

  create_link_banner(parent_); // custom header, so the banner is added explicitly (see buildHeader)

  // Read-only §12 curve preview (requested vs achievable) with phase separators, phase names, axis
  // ticks, and UV shading. Local arrays — the widget copies.
  profile_facts::CurvePoint req[profile_facts::kMaxCurvePoints];
  profile_facts::CurvePoint over[profile_facts::kMaxCurvePoints];
  float bounds[profile_facts::kMaxCurvePhases];
  profile_facts::TimeSpan uv[kMaxPhases];
  const size_t nr = current_->sampleRequested(req, profile_facts::kMaxCurvePoints);
  const size_t no = current_->sampleOvershoot(over, profile_facts::kMaxCurvePoints);
  const size_t nb = current_->samplePhaseBoundaries(bounds, profile_facts::kMaxCurvePhases);
  const size_t nuv = current_->sampleUvSpans(uv, kMaxPhases);
  // Phase labels: one per AUTHORED phase (its stored name). The implicit passive cool-down the
  // samplers append when the run ends hot (implicit_cool.h, §6) is a system safety phase, not
  // operator-authored — it gets no label; its separator and the end-time label carry the right
  // edge.
  char namebuf[profile_facts::kMaxCurvePhases][kPhaseNameCap];
  const char *names[profile_facts::kMaxCurvePhases];
  const size_t authored = current_->phaseNames(namebuf, profile_facts::kMaxCurvePhases);
  for (size_t i = 0; i < authored && i < nb; ++i) {
    names[i] = namebuf[i];
  }
  // Cool phase starts where the last authored phase ends — lets the widget compress the long
  // passive cool-down of a hot reflow rather than squishing the authored phases into the left edge.
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
  cd.n_phase_names = authored; // authored phases only — the implicit cool is unlabelled
  cd.cool_start = cool_start;
  cd.uncalibrated = false; // shown as a warning bar below the graph instead of a label on it
  create_profile_curve(parent_, cd);

  // Uncalibrated warning as an amber bar under the graph (mirrors the editor), rather than a label
  // drawn over the curve.
  if (current_->uncalibrated()) {
    lv_obj_t *banner = lv_obj_create(parent_);
    theme::apply_alert(banner, theme::WARN);
    lv_obj_set_width(banner, lv_pct(100));
    lv_obj_set_height(banner, theme::BANNER_H);
    lv_obj_t *lbl = lv_label_create(banner);
    lv_label_set_text(lbl, "UNCALIBRATED");
    lv_obj_center(lbl);
  }

  // Key facts: "peak 245° · ~6:10 · 4 phases".
  const profile_facts::ProfileFacts f = current_->facts();
  char peak[16];
  char dur[16];
  char facts[48];
  profile_facts::formatPeak(f.peakC, current_->fahrenheit(), peak, sizeof(peak));
  profile_facts::formatDuration(f.totalSeconds, dur, sizeof(dur));
  std::snprintf(facts, sizeof(facts), "%s \xC2\xB7 %s \xC2\xB7 %u phases", peak, dur,
                static_cast<unsigned>(f.phaseCount));
  lv_obj_t *facts_label = lv_label_create(parent_);
  lv_label_set_text(facts_label, facts);
  theme::apply_caption(facts_label);

  // Action row (managing profiles only — running one is a separate path, Home → UV Cure / Reflow →
  // Setup, §19, so there is no Load here). A STOCK profile is read-only: it shows just Delete
  // (greyed) · Clone — Clone is the way to fork it into an editable user copy, so a separate
  // "Save as" would be redundant. A USER profile shows Delete · Rename · Clone · Edit; Edit is
  // rightmost (the most common action, under the finger that just tapped Open), the destructive
  // Delete at the far left. Rename/Edit are hidden for stock (nothing to rename or edit in place).
  const bool user = !current_->rowStock(selected_);
  lv_obj_t *row = lv_obj_create(parent_);
  theme::apply_row(row);
  lv_obj_set_width(row, lv_pct(100));
  lv_obj_set_height(row, theme::SECONDARY_H);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  action_button(row, "Delete", ProfileThunks::delete_evt, this, current_->canDelete(selected_));
  if (user) {
    action_button(row, "Rename", ProfileThunks::rename_evt, this, true);
  }
  action_button(row, "Clone", ProfileThunks::dup_evt, this, true);
  if (user) {
    action_button(row, "Edit", ProfileThunks::edit_evt, this, true);
  }
}

// --- Detail actions ---

void ProfileLibraryScreen::onNew() {
  publishNav(NAV_PROFILE_NEW); // → editor (C5); observed only by tests until it lands
}

void ProfileLibraryScreen::onEdit() {
  publishNav(NAV_PROFILE_EDIT); // → editor / Save-as (C5)
}

void ProfileLibraryScreen::onDuplicate() {
  return_page_ = Page::List;
  // Clone on the controller ("<name> copy", deconflicted against the cached list); poll() re-lists.
  if (current_->requestDuplicate(static_cast<size_t>(selected_))) {
    page_ = Page::Loading;
    pending_ = Pending::Action;
    buildLoading("Working...");
  } else {
    page_ = Page::Error;
    buildError();
  }
}

void ProfileLibraryScreen::onRenameRequested() {
  page_ = Page::Rename;
  buildRename();
}

void ProfileLibraryScreen::onRenameCommit(const char *text) {
  // Rename on the controller; poll() re-lists on success (the name changed, so the highlight's row
  // may have moved under the alphabetical sort). A refusal (empty/invalid/taken name) comes back as
  // a NAK → the error page; Back returns to the list to try again.
  // Reject an empty or already-taken name client-side so the operator stays on the keyboard to pick
  // another (the profile keeps its old name); the controller validates everything else.
  if (text == nullptr || text[0] == '\0' || current_->nameExists(text)) {
    return;
  }
  return_page_ = Page::List;
  if (current_->requestRename(static_cast<size_t>(selected_), text)) {
    page_ = Page::Loading;
    pending_ = Pending::Action;
    buildLoading("Working...");
  }
  // else: client busy — stay on the keyboard (the profile keeps its old name).
}

void ProfileLibraryScreen::buildRename() {
  clearParent();
  configParent();
  buildHeader("Rename");

  rename_ta_ = lv_textarea_create(parent_);
  lv_textarea_set_one_line(rename_ta_, true);
  lv_textarea_set_max_length(rename_ta_, static_cast<uint32_t>(kProfileNameCap - 1));
  lv_textarea_set_placeholder_text(rename_ta_, "Profile name");
  lv_textarea_set_text(rename_ta_, current_->name(selected_)); // prefilled with the current name
  lv_obj_set_width(rename_ta_, lv_pct(100));

  // A flex-grow spacer pins the keyboard to the bottom, the field under the header (editor idiom).
  lv_obj_t *spacer = lv_obj_create(parent_);
  lv_obj_remove_style_all(spacer);
  lv_obj_set_width(spacer, lv_pct(100));
  lv_obj_set_flex_grow(spacer, 1);
  lv_obj_remove_flag(spacer, LV_OBJ_FLAG_CLICKABLE);

  name_kb::create(parent_, rename_ta_, ProfileThunks::rename_ok, this);
}

void ProfileLibraryScreen::onDeleteRequested() {
  page_ = Page::ConfirmDelete;
  char msg[64];
  std::snprintf(msg, sizeof(msg), "Delete \"%s\"?", current_->name(selected_));
  // Overlay on top of the still-built detail page; cancel rebuilds detail, confirm deletes + lists.
  create_confirm_dialog(parent_, msg, "Delete", ProfileThunks::confirm_delete,
                        ProfileThunks::cancel_delete, this);
}

void ProfileLibraryScreen::onDeleteConfirmed() {
  return_page_ = Page::List;
  // Delete on the controller; poll() re-lists. buildList() clamps the selection if the row is gone.
  if (current_->requestRemove(static_cast<size_t>(selected_))) {
    selected_ = 0; // the row is going away; reset the remembered highlight
    page_ = Page::Loading;
    pending_ = Pending::Action;
    buildLoading("Working...");
  } else {
    page_ = Page::Error;
    buildError();
  }
}
