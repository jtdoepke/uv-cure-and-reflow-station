// Settings hub + panels (§24, backlog C8 — Settings slice). The rarely-visited hub for
// preferences + maintenance: a categorized hub → sub-panels, all built from the glove-safe
// selectable list and the two shared numeric editors so nothing relies on precise small taps.
//
// This is a self-contained navigation controller (no global screen manager yet — that arrives
// with C4/C6): it owns a `parent` and rebuilds that parent's children as the user moves hub →
// panel → editor → back, honoring §24's "create-on-demand, delete on leave (no PSRAM)". It owns
// the reused editor view models + list models as members, so their subjects outlive each rebuilt
// editor. Buildable panels only (Display & units, Temperature limits, About, and
// the Advanced master toggle); Network / Data & firmware / Profiles show as disabled "coming
// soon" rows until their backends land.
//
// The controller is the view-model too: it writes the SettingsStore on each change, persists, and
// republishes the cross-screen subjects (subjects.h). Entry is idle-only — the caller (Home)
// gates that. Keep one instance alive for the life of the settings session.
//
// LVGL-only; compiles for firmware and the native_ui_cyd / native_sim host targets.
#pragma once

#include <lvgl.h>

#include "management_client.h"
#include "numeric_keypad_viewmodel.h"
#include "selectable_list.h"
#include "settings_store.h"
#include "value_stepper_viewmodel.h"

enum class SettingsPage {
  Hub,
  DisplayUnits,
  TempLimits,
  Profiles,
  About,
  Editor,
};

// Publish the store's cross-screen values (units, temp caps, Advanced) into the shared subjects
// (subjects.h). Call at boot after SettingsStore::load() so consumers like the profile editor see
// the persisted values before the Settings screen is ever opened. Requires ui_subjects_init().
void settings_publish_subjects(const SettingsStore &store);

class SettingsScreen {
public:
  // Build the hub under `parent`, editing `store`. Both must outlive this screen. Publishes the
  // store's cross-screen values into the shared subjects first. Call after lv_init() +
  // ui_subjects_init().
  //
  // `client` is the shared remote client (§9) the Profiles panel issues its §24 restore through,
  // and it too must outlive this screen. Optional: passing nullptr leaves the Profiles row
  // disabled, which is what the host UI tests and the simulator do — everything else on this
  // screen works without it.
  void begin(lv_obj_t *parent, SettingsStore &store, ManagementClient *client = nullptr);

  // Drive the §24 restore round-trip (busy -> done/failed). Call every loop iteration, alongside
  // the other screens' polls. No-op unless a restore is in flight.
  void poll();

  // Navigation — also the targets of the on-screen Back / Open / editor handlers.
  void showPage(SettingsPage page); // rebuild `parent` with the given panel (not Editor)
  void openHubItem(int index);      // open a category, or toggle Advanced
  void back();                      // Editor → its panel · panel → hub · hub → exit handler
  void confirmRestore();            // §24 restore confirm dialog: Yes — issue the request
  void cancelRestore();             // ...and No — dismiss, restore nothing

  SettingsPage page() const { return page_; }

  // Live brightness-bias preview (§18): true while the brightness-bias stepper editor is open, so
  // the firmware loop can drive the backlight from the in-progress value (liveBrightnessBias())
  // before the user commits — you see the trim as you dial it. Off (falls back to the stored
  // value) the instant the editor commits or cancels.
  bool isEditingBrightnessBias() const {
    return page_ == SettingsPage::Editor && editing_ == EditField::BrightnessBias;
  }
  int32_t liveBrightnessBias() { return stepper_vm_.value(); }

  // The same live-preview seam for the absolute screen-brightness field a no-sensor board shows
  // instead (§18). Preview matters more here than for the bias: this control IS the brightness, so
  // without it you would be dialling a number and hoping.
  bool isEditingScreenBrightness() const {
    return page_ == SettingsPage::Editor && editing_ == EditField::ScreenBrightness;
  }
  int32_t liveScreenBrightness() { return stepper_vm_.value(); }

  // Exit seam: fired when Back is pressed on the hub. Home is the caller's to rebuild.
  void setExitHandler(void (*cb)(void *user_data), void *user_data);

  // --- Inspection accessors (for tests). One list model serves every panel (only one is shown at
  // a time); the two editor VMs back the open editor. ---
  SelectableListModel &listModel() { return list_model_; }
  ValueStepperViewModel &stepperVm() { return stepper_vm_; }
  NumericKeypadViewModel &keypadVm() { return keypad_vm_; }

private:
  // Which store field an open editor edits (drives commit routing).
  enum class EditField { None, BrightnessBias, ScreenBrightness, IdleTimeout, UvCap, ReflowCap };

  // What a row of the Display & units panel does. The panel's rows are built conditionally (a
  // board with no light sensor has no auto-brightness row at all), so the row INDEX no longer
  // identifies the action — Open must dispatch on what was actually built, not on a number.
  // Storing the mapping is what stops index 1 meaning "auto-brightness" on one board and
  // "screen brightness" on another.
  enum class DisplayRow { Units, AutoBrightness, Brightness, IdleTimeout };

  // Panel builders (each clears `parent_` and lays out its own content). Every actionable panel is
  // a selectable list (scroll + Up/Down + a per-row verb); About is a read-only scrolling column.
  void buildHub();
  void buildDisplayUnits();
  void buildTempLimits();
  void buildProfiles();
  void buildAbout();

  // Per-panel Open dispatchers (the list's open seam; recover `this` via user_data thunks).
  void onDisplayOpen(int index);
  void onTempOpen(int index);
  void onProfilesOpen(int index);

  // Re-grey the hub's editable rows when the controller link flips (§9): the settings live on the
  // controller now, so a dropped link greys the category rows (About, local info, stays open). Only
  // rebuilds on an actual OK↔down transition while the hub is showing — never per-loop.
  void onLinkChanged();

  // Shared bits.
  void buildHeader(const char *title); // header row (< Back + title) into `parent_`
  void openEditor(EditField field, SettingsPage return_page); // title/config derived from field
  void configParent();
  void commitEditor(int32_t value);
  void publishToSubjects();
  void clearParent();
  void reselect(int index); // rebuild-and-reselect after an in-place toggle/cycle

  lv_obj_t *parent_ = nullptr;
  SettingsStore *store_ = nullptr;
  ManagementClient *client_ = nullptr; // §24 restore; nullptr disables the Profiles row
  SettingsPage page_ = SettingsPage::Hub;

  // §24 "Restore stock profiles" round-trip state. A plain enum rather than a view-model: it is
  // one request with one verdict, shown on the panel that issued it.
  enum class Restore : uint8_t { Idle, Confirming, Busy, Done, Failed };
  Restore restore_ = Restore::Idle;
  oven_Mode restore_mode_ = oven_Mode_MODE_UNSPECIFIED;
  bool last_link_ok_ = true; // last link gate applied to the hub — only rebuild when it flips

  SettingsPage editor_return_ = SettingsPage::Hub;
  EditField editing_ = EditField::None;

  // Reused models/VMs — members so their subjects survive each rebuild.
  SelectableListModel list_model_;
  ValueStepperViewModel stepper_vm_;
  NumericKeypadViewModel keypad_vm_;

  // Formatted value strings for the current page; members so the borrowed pointers the list rows
  // hold stay valid while that page is shown.
  char uv_value_[16]{};
  char reflow_value_[16]{};
  char bias_value_[16]{};
  char idle_value_[16]{};

  // Row index -> action for the currently built Display & units panel (see DisplayRow).
  DisplayRow display_rows_[4]{};
  int display_row_count_ = 0;

  void (*on_exit_)(void *) = nullptr;
  void *exit_ud_ = nullptr;

  friend struct SettingsThunks; // grants the .cpp's captureless thunks access to private methods
};
