#include "settings_screen.h"

#include <cstdint>

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
  HUB_SLEEP = 2,
  HUB_NETWORK = 3,  // disabled — WiFi (D9)
  HUB_DATA_FW = 4,  // disabled — OTA / logs
  HUB_PROFILES = 5, // disabled — ProfileStore (B4)
  HUB_ABOUT = 6,
  HUB_ADVANCED = 7,
  HUB_COUNT = 8,
};

// A read-only informational row: label left, value/detail right (About; the fixed sleep rules).
void build_info_row(lv_obj_t *parent, const char *label, const char *value) {
  lv_obj_t *row = lv_obj_create(parent);
  theme::apply_list_row(row);
  lv_obj_set_width(row, lv_pct(100));
  lv_obj_set_height(row, theme::LIST_ROW_H);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_t *name = lv_label_create(row);
  lv_label_set_text(name, label);
  if (value != nullptr) {
    lv_obj_t *val = lv_label_create(row);
    lv_label_set_text(val, value);
    lv_obj_set_style_text_color(val, theme::col(theme::TEXT_DIM), 0);
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
  static void sleep_open(int index, void *ud) {
    static_cast<SettingsScreen *>(ud)->onSleepOpen(index);
  }
  static void temp_open(int index, void *ud) {
    static_cast<SettingsScreen *>(ud)->onTempOpen(index);
  }
  static void back_evt(lv_event_t *e) {
    static_cast<SettingsScreen *>(lv_event_get_user_data(e))->back();
  }
  static void editor_commit(int32_t value, void *ud) {
    static_cast<SettingsScreen *>(ud)->commitEditor(value);
  }
  static void editor_cancel(void *ud) { static_cast<SettingsScreen *>(ud)->back(); }
};

// --- Lifecycle ---

void SettingsScreen::begin(lv_obj_t *parent, SettingsStore &store) {
  parent_ = parent;
  store_ = &store;
  publishToSubjects();
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
  case SettingsPage::SleepWake:
    buildSleepWake();
    break;
  case SettingsPage::About:
    buildAbout();
    break;
  case SettingsPage::Editor:
    break; // editors are built via openEditor(), never showPage()
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
  case HUB_SLEEP:
    showPage(SettingsPage::SleepWake);
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
}

// --- Panels (each is a selectable list: scroll + Up/Down + a per-row verb) ---

void SettingsScreen::buildHub() {
  configParent();
  buildHeader("Settings");

  const char *advanced_value = store_->advancedUnlocked() ? "On" : "Off";
  const SelectableListItem items[HUB_COUNT] = {
      {"Display & units", nullptr, true, "Open"}, {"Temperature limits", nullptr, true, "Open"},
      {"Sleep & wake", nullptr, true, "Open"},    {"Network (WiFi)", "soon", false},
      {"Data & firmware", "soon", false},         {"Profiles", "soon", false},
      {"About", nullptr, true, "Open"},           {"Advanced", advanced_value, true, "Toggle"},
  };
  list_model_.init(items, HUB_COUNT, /*wrap=*/true);
  list_model_.setOpenHandler(SettingsThunks::hub_open, this);
  create_selectable_list(parent_, list_model_);
}

void SettingsScreen::buildDisplayUnits() {
  configParent();
  buildHeader("Display & units");
  const char *units_value = store_->units() == TempUnits::Fahrenheit ? "Fahrenheit" : "Celsius";
  // Auto-brightness needs a sensor. On a board with none the row stays visible but disabled —
  // SelectableListModel skips disabled rows for ▲/▼ and refuses to open them — because a row that
  // says "Not fitted" answers the question, while a row that silently vanishes invites someone to
  // go looking for the setting they remember. The stored preference is untouched either way.
  const bool has_ldr = lv_subject_get_int(&subj_has_ambient_light) != 0;
  const char *auto_value = !has_ldr ? "Not fitted" : (store_->autoBrightness() ? "On" : "Off");
  SettingsStore::brightnessBiasConfig().format(store_->brightnessBias(), bias_value_,
                                               sizeof(bias_value_));
  const SelectableListItem items[3] = {
      {"Temperature units", units_value, true, "Change"},
      {"Auto-brightness", auto_value, has_ldr, "Toggle"},
      {"Brightness bias", bias_value_, true, "Edit"},
  };
  list_model_.init(items, 3, /*wrap=*/true);
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

void SettingsScreen::buildSleepWake() {
  configParent();
  buildHeader("Sleep & wake");
  SettingsStore::idleTimeoutConfig().format(store_->idleTimeoutMin(), idle_value_,
                                            sizeof(idle_value_));
  // The never-sleep-during-a-run / stay-awake-while-HOT rules are fixed, not user-disableable
  // (§17): shown as disabled rows so Up/Down skip them and Open can't act on them.
  const SelectableListItem items[3] = {
      {"Idle timeout", idle_value_, true, "Edit"},
      {"Never sleeps during a run", "fixed", false},
      {"Stays awake while HOT", "fixed", false},
  };
  list_model_.init(items, 3, /*wrap=*/true);
  list_model_.setOpenHandler(SettingsThunks::sleep_open, this);
  create_selectable_list(parent_, list_model_);
}

void SettingsScreen::buildAbout() {
  configParent();
  buildHeader("About");
  // Read-only, so no Up/Down/Open footer — just a scrolling column of info rows. Real values
  // arrive with the controller-link + handshake wiring (§9); placeholders for now.
  lv_obj_t *col = make_scroll_column(parent_);
  build_info_row(col, "CYD firmware", "dev");
  build_info_row(col, "Controller", "-");
  build_info_row(col, "Schema hash", "-");
  build_info_row(col, "Board", "ESP32-2432S028");
}

// --- Per-panel Open dispatchers ---

void SettingsScreen::onDisplayOpen(int index) {
  switch (index) {
  case 0: // Temperature units — a two-value enum; Open cycles it (no checkmark, §24).
    store_->setUnits(store_->units() == TempUnits::Celsius ? TempUnits::Fahrenheit
                                                           : TempUnits::Celsius);
    store_->save();
    publishToSubjects(); // subj_units → Home re-renders the chamber temp in the new unit
    reselect(0);
    break;
  case 1: // Auto-brightness — a boolean; Open toggles it.
    store_->setAutoBrightness(!store_->autoBrightness());
    store_->save();
    reselect(1);
    break;
  case 2: // Brightness bias — a nudge field; Open edits it.
    openEditor(EditField::BrightnessBias, SettingsPage::DisplayUnits);
    break;
  default:
    break;
  }
}

void SettingsScreen::onSleepOpen(int index) {
  if (index == 0) {
    openEditor(EditField::IdleTimeout, SettingsPage::SleepWake);
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
