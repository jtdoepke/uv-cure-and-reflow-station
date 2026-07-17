// profile_library_screen — the §23 profile library screen pair (backlog C4).
//
// A self-contained hub-and-spoke controller (the SettingsScreen pattern): it owns a `parent` and
// rebuilds it as the user moves mode-chooser → mode-scoped list → profile detail → delete-confirm,
// create-on-demand (no PSRAM). Reached from Home → Profiles (NAV_PROFILES); the entry is
// mode-blind, so a small chooser picks Cure or Reflow, then the fixed-mode library is shown (§23:
// never mixed).
//
// The two per-mode ProfileLibraryViewModels (pure app_logic) supply the rows/facts/actions; this
// file is the view + navigation. Store-mutating actions (Dup, Delete) run now against the store;
// New/Edit lead to the editor (§12/C5). The Profiles branch is for MANAGING profiles only — running
// one is a separate path (Home → UV Cure / Reflow → Setup, §19), so this screen has no Load.
//
// LVGL-only; compiles for firmware and the native_ui_cyd / native_sim host targets.
#pragma once

#include <lvgl.h>

#include "oven_cal.h"
#include "profile_library_viewmodel.h"
#include "selectable_list.h"

class ProfileLibraryScreen {
public:
  enum class Page { Chooser, List, Detail, ConfirmDelete };

  // Build the chooser under `parent`, over the two per-mode stores. All three must outlive this
  // screen. Call after lv_init() + ui_subjects_init(). `model` defaults to the compiled-in
  // calibration; a test may pass a toy model.
  void begin(lv_obj_t *parent, ProfileStore &cure, ProfileStore &reflow,
             const OvenModel &model = oven_cal::kDefaultModel);

  // Exit seam: fired when Back is pressed on the chooser (Home is the caller's to rebuild).
  void setExitHandler(void (*cb)(void *user_data), void *user_data);

  // Navigation + button targets.
  void showChooser();
  void openMode(RecipeMode mode); // chooser → that mode's list
  void openDetail(int index);     // list Open → detail
  void back(); // confirm → detail · detail → list · list → chooser · chooser → exit

  // Detail actions.
  void onNew();       // → editor on a fresh template (NAV_PROFILE_NEW)
  void onEdit();      // → editor on the selected profile (Save-as for stock) (NAV_PROFILE_EDIT)
  void onDuplicate(); // Dup within this library, then re-list
  void onDeleteRequested(); // → the confirm dialog
  void onDeleteConfirmed(); // Delete, then back to the list

  // Inspection (tests).
  Page page() const { return page_; }
  RecipeMode mode() const { return mode_; }
  int selected() const { return selected_; }
  SelectableListModel &listModel() { return list_model_; }
  ProfileLibraryViewModel &vm() { return *current_; }

private:
  void buildChooser();
  void buildList();
  void buildDetail();
  void buildConfirm();
  void buildHeader(const char *title); // < Back + title into `parent_`
  void configParent();
  void clearParent();
  void publishNav(int nav_request); // set subj_nav_request (the deferred-destination seam)

  lv_obj_t *parent_ = nullptr;
  Page page_ = Page::Chooser;
  RecipeMode mode_ = RecipeMode::Reflow;
  int selected_ = 0; // remembered highlight, restored when returning list → (detail →) list

  ProfileLibraryViewModel cure_vm_;
  ProfileLibraryViewModel reflow_vm_;
  ProfileLibraryViewModel *current_ = nullptr; // the shown mode's VM (&cure_vm_ / &reflow_vm_)
  SelectableListModel list_model_;             // the mode-scoped list (the chooser is two tiles)

  void (*on_exit_)(void *) = nullptr;
  void *exit_ud_ = nullptr;

  friend struct ProfileThunks; // grants the .cpp's captureless thunks access to private methods
};
