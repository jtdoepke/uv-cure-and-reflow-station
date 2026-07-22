#include "settings_screen.h"

#include <cstdint>

#include "confirm_dialog.h" // §24 Restore stock profiles — a simple confirm, not §19's hold
#include "device_info.h"
#include "link_banner.h" // shared "Controller not responding" banner (§9/§14)
#include "numeric_keypad.h"
#include "subjects.h"
#include "theme.h"
#include "value_stepper.h"

// Hub row order (index -> action). Fixed so openHubItem() can switch on the index; kept beside the
// buildHub() item table so the two never drift.
namespace {
enum HubIndex {
  HUB_DISPLAY = 0,
  HUB_TEMP = 1,
  HUB_NETWORK = 2,  // disabled — WiFi (D9)
  HUB_DATA_FW = 3,  // disabled — OTA / logs
  HUB_PROFILES = 4, // §24 Restore stock profiles (needs the remote client, §9)
  HUB_ABOUT = 5,
  HUB_ADVANCED = 6,
  HUB_COUNT = 7,
};

// A read-only informational row: label left, value/detail right (About; the fixed sleep rules).
// Shares the selectable list's anti-collision geometry (theme::apply_labeled_row). This panel
// needs it most: About's values are the long ones ("320x480 portrait", a schema hash).
void build_info_row(lv_obj_t *parent, const char *label, const char *value) {
  lv_obj_t *row = lv_obj_create(parent);
  theme::apply_list_row(row);
  theme::apply_labeled_row(row);
  lv_obj_t *name = lv_label_create(row);
  lv_label_set_text(name, label);
  lv_obj_set_flex_grow(name, 1);
  lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
  if (value != nullptr) {
    lv_obj_t *val = lv_label_create(row);
    lv_label_set_text(val, value);
    lv_obj_set_style_text_color(val, theme::col(theme::TEXT_DIM), 0);
    // Natural width, no wrap — see create_selectable_list() on why a max_width here clips.
  }
}

// A scrolling column filling the space below the header — for read-only panels (About) that have
// no Up/Down/Open footer but still must never overflow the screen.
lv_obj_t *make_scroll_column(lv_obj_t *parent) {
  lv_obj_t *col = lv_obj_create(parent);
  theme::apply_row(col); // transparent container
  lv_obj_set_width(col, lv_pct(100));
  lv_obj_set_flex_grow(col, 1);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(col, theme::PAD_S, 0);
  lv_obj_add_flag(col, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(col, LV_DIR_VER);
  return col;
}
} // namespace

// Captureless thunks for LVGL/seam callbacks. A friend of SettingsScreen so it can reach the
// private navigation/commit methods and the private EditField enum (matches the codebase's
// single-void*-user_data idiom — no std::function).
struct SettingsThunks {
  static void hub_open(int index, void *ud) {
    static_cast<SettingsScreen *>(ud)->openHubItem(index);
  }
  static void display_open(int index, void *ud) {
    static_cast<SettingsScreen *>(ud)->onDisplayOpen(index);
  }
  static void temp_open(int index, void *ud) {
    static_cast<SettingsScreen *>(ud)->onTempOpen(index);
  }
  static void profiles_open(int index, void *ud) {
    static_cast<SettingsScreen *>(ud)->onProfilesOpen(index);
  }
  static void restore_confirm(void *ud) { static_cast<SettingsScreen *>(ud)->confirmRestore(); }
  static void restore_cancel(void *ud) { static_cast<SettingsScreen *>(ud)->cancelRestore(); }
  static void back_evt(lv_event_t *e) {
    static_cast<SettingsScreen *>(lv_event_get_user_data(e))->back();
  }
  static void editor_commit(int32_t value, void *ud) {
    static_cast<SettingsScreen *>(ud)->commitEditor(value);
  }
  static void editor_cancel(void *ud) { static_cast<SettingsScreen *>(ud)->back(); }
  static void link_changed(lv_observer_t *o, lv_subject_t *) {
    static_cast<SettingsScreen *>(lv_observer_get_user_data(o))->onLinkChanged();
  }
};

// --- Lifecycle ---

void SettingsScreen::begin(lv_obj_t *parent, SettingsStore &store, ManagementClient *client) {
  parent_ = parent;
  store_ = &store;
  client_ = client;
  restore_ = Restore::Idle;
  publishToSubjects();
  // Re-grey the hub's category rows when the link flips (§9). Tied to parent_ so it is removed with
  // the screen (create-on-demand); seed last_link_ok_ from the current state so the first (possibly
  // same-value) notification is a no-op rather than a spurious rebuild.
  last_link_ok_ = lv_subject_get_int(&subj_link_state) == LINK_OK;
  lv_subject_add_observer_obj(&subj_link_state, SettingsThunks::link_changed, parent_, this);
  showPage(SettingsPage::Hub);
}

void SettingsScreen::setExitHandler(void (*cb)(void *), void *user_data) {
  on_exit_ = cb;
  exit_ud_ = user_data;
}

void settings_publish_subjects(const SettingsStore &store) {
  lv_subject_set_int(&subj_units, store.units() == TempUnits::Fahrenheit ? 1 : 0);
  lv_subject_set_int(&subj_uv_cap, store.uvMaxCap());
  lv_subject_set_int(&subj_reflow_cap, store.reflowMaxCap());
  lv_subject_set_int(&subj_advanced, store.advancedUnlocked() ? 1 : 0);
}

void SettingsScreen::publishToSubjects() {
  settings_publish_subjects(*store_);
}

// --- Navigation ---

void SettingsScreen::showPage(SettingsPage page) {
  clearParent();
  page_ = page;
  switch (page) {
  case SettingsPage::Hub:
    buildHub();
    break;
  case SettingsPage::DisplayUnits:
    buildDisplayUnits();
    break;
  case SettingsPage::TempLimits:
    buildTempLimits();
    break;
  case SettingsPage::Profiles:
    buildProfiles();
    break;
  case SettingsPage::About:
    buildAbout();
    break;
  case SettingsPage::Editor:
    break; // editors are built via openEditor(), never showPage()
  }
}

void SettingsScreen::onLinkChanged() {
  const bool ok = lv_subject_get_int(&subj_link_state) == LINK_OK;
  if (ok == last_link_ok_) {
    return; // gate unchanged — nothing to re-grey
  }
  last_link_ok_ = ok;
  // Only the hub carries the gated rows; a sub-panel/editor keeps its banner and re-gates on Back
  // (showPage rebuilds the hub then). This runs inside the subj_link_state notification and
  // rebuilds the hub — which deletes+recreates the banner (also a subj_link_state observer). That
  // is safe: LVGL's lv_subject_notify restarts on an observer add/remove, and the guard above makes
  // this callback a no-op on that restart, so it settles in one rebuild rather than looping.
  if (page_ == SettingsPage::Hub) {
    showPage(SettingsPage::Hub);
  }
}

void SettingsScreen::reselect(int index) {
  showPage(page_); // rebuild the current panel so a changed value re-renders
  list_model_.select(index);
}

void SettingsScreen::openHubItem(int index) {
  switch (index) {
  case HUB_DISPLAY:
    showPage(SettingsPage::DisplayUnits);
    break;
  case HUB_TEMP:
    showPage(SettingsPage::TempLimits);
    break;
  case HUB_PROFILES:
    restore_ = Restore::Idle; // entering fresh: don't re-show a previous verdict
    showPage(SettingsPage::Profiles);
    break;
  case HUB_ABOUT:
    showPage(SettingsPage::About);
    break;
  case HUB_ADVANCED:
    // Advanced is a master toggle living in the hub list (§24): Open flips it, then the hub is
    // rebuilt so the row's On/Off value refreshes; keep the highlight on it.
    store_->setAdvancedUnlocked(!store_->advancedUnlocked());
    store_->save();
    publishToSubjects();
    reselect(HUB_ADVANCED);
    break;
  default:
    break; // disabled rows never open
  }
}

void SettingsScreen::back() {
  switch (page_) {
  case SettingsPage::Editor:
    showPage(editor_return_); // discard: the editor's Cancel already restored its working copy
    break;
  case SettingsPage::Hub:
    if (on_exit_ != nullptr) {
      on_exit_(exit_ud_);
    }
    break;
  default:
    showPage(SettingsPage::Hub); // any panel returns to the hub
    break;
  }
}

// --- Shared building blocks ---

void SettingsScreen::clearParent() {
  lv_obj_clean(parent_);
}

void SettingsScreen::configParent() {
  theme::apply_screen(parent_);
  lv_obj_set_flex_flow(parent_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(parent_, theme::PAD_S, 0);
  lv_obj_set_style_pad_row(parent_, theme::GAP, 0);
}

void SettingsScreen::buildHeader(const char *title) {
  lv_obj_t *header = lv_obj_create(parent_);
  theme::apply_panel(header);
  lv_obj_set_width(header, lv_pct(100));
  lv_obj_set_height(header, theme::SECONDARY_H); // tall enough that Back is a real touch target
  lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_gap(header, theme::PAD_M, 0);

  lv_obj_t *back = lv_button_create(header);
  theme::apply_secondary(back);
  lv_obj_set_height(back, lv_pct(100));
  lv_obj_t *back_label = lv_label_create(back);
  lv_label_set_text(back_label, LV_SYMBOL_LEFT " Back");
  lv_obj_center(back_label);
  lv_obj_add_event_cb(back, SettingsThunks::back_evt, LV_EVENT_CLICKED, this);

  lv_obj_t *title_label = lv_label_create(header);
  lv_label_set_text(title_label, title);

  // Settings persist on the controller now (§7): the CYD is a remote client, so surface a dropped
  // link here too. Edits keep working against the local cache and sync on reconnect (main.cpp), so
  // this is a banner, not a lockout — it just says why a change may not have landed yet.
  create_link_banner(parent_);
}

// --- Panels (each is a selectable list: scroll + Up/Down + a per-row verb) ---

void SettingsScreen::buildHub() {
  configParent();
  buildHeader("Settings");

  const char *advanced_value = store_->advancedUnlocked() ? "On" : "Off";
  // The settings live on the controller now (§7), so the editable categories gate on the link like
  // every other management action: greyed + unopenable when down. About is local device info, so it
  // stays open; the "soon" rows stay disabled regardless. onLinkChanged() rebuilds this on a flip.
  const bool ok = lv_subject_get_int(&subj_link_state) == LINK_OK;
  const SelectableListItem items[HUB_COUNT] = {
      {"Display & units", nullptr, ok, "Open"},
      {"Temperature limits", nullptr, ok, "Open"},
      {"Network (WiFi)", "soon", false},
      {"Data & firmware", "soon", false},
      // Profiles is a real panel now (§24 Restore stock profiles) but still needs the remote
      // client: the library lives on the controller (§7), so with no client there is nothing to
      // restore and the row reads "soon" like the genuinely-unbuilt ones.
      {"Profiles", client_ == nullptr ? "soon" : nullptr, client_ != nullptr && ok, "Open"},
      {"About", nullptr, true, "Open"},
      {"Advanced", advanced_value, ok, "Toggle"},
  };
  list_model_.init(items, HUB_COUNT, /*wrap=*/true);
  list_model_.setOpenHandler(SettingsThunks::hub_open, this);
  create_selectable_list(parent_, list_model_);
}

void SettingsScreen::buildDisplayUnits() {
  configParent();
  buildHeader("Display & units");
  const char *units_value = store_->units() == TempUnits::Fahrenheit ? "Fahrenheit" : "Celsius";
  // Both brightness rows depend on whether this board has a light sensor — a capability that
  // arrives as DATA (subj_has_ambient_light); this file must never see a board flag.
  //
  // With no sensor the auto-brightness row is OMITTED, not shown-and-disabled. It was disabled-
  // but-visible at first, on the theory that "Not fitted" answers the question a vanished row
  // invites someone to go looking for. That was wrong for this screen: a settings list is a list
  // of things you can change, and a row that can never change on this hardware is furniture —
  // it costs a line of a small screen, it draws the eye, and ▲/▼ skip past it anyway. (Rows that
  // are merely not-yet-changeable, like the hub's "soon" ones, are a different case: those become
  // real, so they stay.)
  //
  // The brightness row below then swaps meaning rather than disappearing: with a sensor it is a
  // BIAS (a +/- trim on the ambient reading), without one it is the plain absolute level, because
  // a trim on a constant is an indirection with no second term.
  const bool has_ldr = lv_subject_get_int(&subj_has_ambient_light) != 0;
  const char *bright_label = has_ldr ? "Brightness bias" : "Screen brightness";
  if (has_ldr) {
    SettingsStore::brightnessBiasConfig().format(store_->brightnessBias(), bias_value_,
                                                 sizeof(bias_value_));
  } else {
    SettingsStore::screenBrightnessConfig().format(store_->screenBrightnessPct(), bias_value_,
                                                   sizeof(bias_value_));
  }

  SelectableListItem items[4];
  int n = 0;
  display_rows_[n] = DisplayRow::Units;
  items[n++] = {"Temperature units", units_value, true, "Change"};
  if (has_ldr) {
    display_rows_[n] = DisplayRow::AutoBrightness;
    items[n++] = {"Auto-brightness", store_->autoBrightness() ? "On" : "Off", true, "Toggle"};
  }
  display_rows_[n] = DisplayRow::Brightness;
  items[n++] = {bright_label, bias_value_, true, "Edit"};

  // Idle timeout lives here rather than in a panel of its own. It used to head a "Sleep & wake"
  // category alongside the two fixed rules; once those went (they are policy, not settings) that
  // panel was one row deep, which is a menu level charging a tap for nothing. It belongs with
  // brightness anyway: both are "what the screen does when you are not touching it" (§17/§18).
  SettingsStore::idleTimeoutConfig().format(store_->idleTimeoutMin(), idle_value_,
                                            sizeof(idle_value_));
  display_rows_[n] = DisplayRow::IdleTimeout;
  items[n++] = {"Idle timeout", idle_value_, true, "Edit"};
  display_row_count_ = n;

  list_model_.init(items, n, /*wrap=*/true);
  list_model_.setOpenHandler(SettingsThunks::display_open, this);
  create_selectable_list(parent_, list_model_);
}

void SettingsScreen::buildTempLimits() {
  configParent();
  buildHeader("Temperature limits");
  SettingsStore::uvCapConfig().format(store_->uvMaxCap(), uv_value_, sizeof(uv_value_));
  SettingsStore::reflowCapConfig().format(store_->reflowMaxCap(), reflow_value_,
                                          sizeof(reflow_value_));
  const SelectableListItem items[2] = {
      {"UV cure max", uv_value_, true, "Edit"},
      {"Reflow max", reflow_value_, true, "Edit"},
  };
  list_model_.init(items, 2, /*wrap=*/true);
  list_model_.setOpenHandler(SettingsThunks::temp_open, this);
  create_selectable_list(parent_, list_model_);
}

// §24's "Restore stock profiles", per mode (§23 puts it here). Two rows rather than one, because
// the libraries are independent (§7 never-mixed) and an operator repairing their cure set has no
// reason to touch reflow.
void SettingsScreen::buildProfiles() {
  configParent();
  buildHeader("Profiles");

  // The verdict from the last attempt, if any — shown above the list, on the panel that issued it.
  // Plain rows, not a modal: nothing here is hazardous and nothing needs acknowledging (§22 keeps
  // the modal rare on purpose).
  switch (restore_) {
  case Restore::Busy:
    build_info_row(parent_, "Restoring...", nullptr);
    break;
  case Restore::Done:
    build_info_row(parent_, "Stock profiles restored", nullptr);
    break;
  case Restore::Failed:
    // Deliberately specific about the likely cause. The controller refuses rather than clobber a
    // user profile that holds a stock name, and an operator told only "failed" would have no way
    // to work out why the stock profile never came back.
    build_info_row(parent_, "Restore failed - a saved profile may be using a stock name", nullptr);
    break;
  case Restore::Idle:
  case Restore::Confirming:
    break;
  }

  const bool ok = client_ != nullptr && lv_subject_get_int(&subj_link_state) == LINK_OK &&
                  restore_ != Restore::Busy;
  const SelectableListItem items[2] = {
      {"Restore cure stock", nullptr, ok, "Restore"},
      {"Restore reflow stock", nullptr, ok, "Restore"},
  };
  list_model_.init(items, 2, /*wrap=*/true);
  list_model_.setOpenHandler(SettingsThunks::profiles_open, this);
  create_selectable_list(parent_, list_model_);

  if (restore_ == Restore::Confirming) {
    // A SIMPLE confirm, not §19's press-and-hold: restoring writes profiles and energizes nothing.
    // Not danger-red either, for the same reason (§13/§22 reserve red for the hazardous verb).
    create_confirm_dialog(
        parent_,
        restore_mode_ == oven_Mode_MODE_CURE
            ? "Reinstall the factory cure profiles? Your own profiles are kept."
            : "Reinstall the factory reflow profiles? Your own profiles are kept.",
        "Restore", SettingsThunks::restore_confirm, SettingsThunks::restore_cancel, this);
  }
}

void SettingsScreen::onProfilesOpen(int index) {
  if (client_ == nullptr || restore_ == Restore::Busy) {
    return;
  }
  restore_mode_ = index == 0 ? oven_Mode_MODE_CURE : oven_Mode_MODE_REFLOW;
  restore_ = Restore::Confirming;
  showPage(SettingsPage::Profiles);
  list_model_.select(index); // keep the highlight where the operator left it
}

void SettingsScreen::confirmRestore() {
  // requestRestoreStock fails only when the single-outstanding slot is busy; treat that as a
  // failed attempt rather than silently dropping it, so the operator can retry.
  restore_ = (client_ != nullptr && client_->requestRestoreStock(restore_mode_)) ? Restore::Busy
                                                                                 : Restore::Failed;
  showPage(SettingsPage::Profiles);
}

void SettingsScreen::cancelRestore() {
  restore_ = Restore::Idle;
  showPage(SettingsPage::Profiles);
}

void SettingsScreen::poll() {
  if (restore_ != Restore::Busy || client_ == nullptr) {
    return;
  }
  if (client_->busy()) {
    return;
  }
  // Only claim success for OUR request: the client is shared, and a reply to someone else's op
  // arriving here must not be read as a restore verdict.
  const bool ours = client_->lastOp() == ManagementClient::Op::RestoreStock;
  restore_ = (ours && client_->ready()) ? Restore::Done : Restore::Failed;
  client_->clear();
  if (page_ == SettingsPage::Profiles) {
    showPage(SettingsPage::Profiles); // render the verdict
  }
}

void SettingsScreen::buildAbout() {
  configParent();
  buildHeader("About");
  // Read-only, so no Up/Down/Open footer — just a scrolling column of info rows. Real values
  // arrive with the controller-link + handshake wiring (§9); placeholders for now.
  // Identity comes from the firmware (device_info.h), never from a literal here: this panel is
  // where someone checks what they are running, so a stale hard-coded board name is not a cosmetic
  // bug — it is this screen failing at its only job. It said "ESP32-2432S028" on the 3.5" board.
  const DeviceInfo &info = ui_device_info();
  lv_obj_t *col = make_scroll_column(parent_);
  build_info_row(col, "CYD firmware", info.firmware);
  build_info_row(col, "Board", info.board);
  build_info_row(col, "Panel", info.panel);
  build_info_row(col, "Controller", "-");
  build_info_row(col, "Schema hash", "-");
}

// --- Per-panel Open dispatchers ---

void SettingsScreen::onDisplayOpen(int index) {
  if (index < 0 || index >= display_row_count_) {
    return;
  }
  switch (display_rows_[index]) { // the built row's action, never the raw index — see DisplayRow
  case DisplayRow::Units:         // a two-value enum; Open cycles it (no checkmark, §24).
    store_->setUnits(store_->units() == TempUnits::Celsius ? TempUnits::Fahrenheit
                                                           : TempUnits::Celsius);
    store_->save();
    publishToSubjects(); // subj_units → Home re-renders the chamber temp in the new unit
    reselect(index);
    break;
  case DisplayRow::AutoBrightness: // a boolean; Open toggles it. Only built when a sensor exists.
    store_->setAutoBrightness(!store_->autoBrightness());
    store_->save();
    reselect(index);
    break;
  case DisplayRow::Brightness: // a nudge field either way; which one depends on the sensor.
    openEditor(lv_subject_get_int(&subj_has_ambient_light) != 0 ? EditField::BrightnessBias
                                                                : EditField::ScreenBrightness,
               SettingsPage::DisplayUnits);
    break;
  case DisplayRow::IdleTimeout: // how long until the screen sleeps (§17).
    openEditor(EditField::IdleTimeout, SettingsPage::DisplayUnits);
    break;
  }
}

void SettingsScreen::onTempOpen(int index) {
  openEditor(index == 0 ? EditField::UvCap : EditField::ReflowCap, SettingsPage::TempLimits);
}

// --- Editors ---

void SettingsScreen::openEditor(EditField field, SettingsPage return_page) {
  NumericFieldConfig cfg{};
  const char *title = "";
  int32_t initial = 0;
  switch (field) {
  case EditField::BrightnessBias:
    cfg = SettingsStore::brightnessBiasConfig();
    title = "Brightness bias";
    initial = store_->brightnessBias();
    break;
  case EditField::ScreenBrightness:
    cfg = SettingsStore::screenBrightnessConfig();
    title = "Screen brightness";
    initial = store_->screenBrightnessPct();
    break;
  case EditField::IdleTimeout:
    cfg = SettingsStore::idleTimeoutConfig();
    title = "Idle timeout";
    initial = store_->idleTimeoutMin();
    break;
  case EditField::UvCap:
    cfg = SettingsStore::uvCapConfig();
    title = "UV cure max";
    initial = store_->uvMaxCap();
    break;
  case EditField::ReflowCap:
    cfg = SettingsStore::reflowCapConfig();
    title = "Reflow max";
    initial = store_->reflowMaxCap();
    break;
  case EditField::None:
    return;
  }

  clearParent();
  editing_ = field;
  editor_return_ = return_page;
  page_ = SettingsPage::Editor;

  // The >20-step rule picks the editor: nudge fields get the stepper, wide fields the keypad.
  if (cfg.usesStepper()) {
    stepper_vm_.init(cfg, initial);
    stepper_vm_.setCommitHandler(SettingsThunks::editor_commit, this);
    stepper_vm_.setCancelHandler(SettingsThunks::editor_cancel, this);
    create_value_stepper(parent_, stepper_vm_, title);
  } else {
    keypad_vm_.init(cfg, initial);
    keypad_vm_.setCommitHandler(SettingsThunks::editor_commit, this);
    keypad_vm_.setCancelHandler(SettingsThunks::editor_cancel, this);
    create_numeric_keypad(parent_, keypad_vm_, title);
  }
}

void SettingsScreen::commitEditor(int32_t value) {
  switch (editing_) {
  case EditField::BrightnessBias:
    store_->setBrightnessBias(value);
    break;
  case EditField::ScreenBrightness:
    store_->setScreenBrightnessPct(value);
    break;
  case EditField::IdleTimeout:
    store_->setIdleTimeoutMin(value);
    break;
  case EditField::UvCap:
    store_->setUvMaxCap(value);
    break;
  case EditField::ReflowCap:
    store_->setReflowMaxCap(value);
    break;
  case EditField::None:
    return;
  }
  store_->save();
  publishToSubjects();
  SettingsPage ret = editor_return_;
  editing_ = EditField::None;
  showPage(ret);
}
