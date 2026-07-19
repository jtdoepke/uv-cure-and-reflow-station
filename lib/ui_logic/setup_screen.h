// setup_screen — the §19 run-setup screen (backlog C6a). Home → UV Cure / Reflow lands here: it is
// where a run is composed and launched, distinct from the Profiles branch (which only MANAGES
// profiles, §23). It owns the run's WORKING COPY — a store-free ProfileDraft (profile_draft.h) —
// and hands it to the picker, the editor, and (C6b) the Confirm screen.
//
// A self-contained hub-and-spoke controller (the SettingsScreen / ProfileEditorScreen pattern): it
// owns a `parent` and rebuilds it as the operator moves Empty ↔ Loaded, create-on-demand (no
// PSRAM). But unlike those screens its SESSION state (the run draft) outlives a rebuild: entering
// from Home starts a fresh session (enterMode clears the draft), while returning from the picker or
// editor only re-renders (setDraft adopts the choice, render rebuilds Loaded) — so the composition
// root separates "new session" (enterMode) from "rebuild current page" (render), like the editor's
// begin*/render split.
//
// The actions route through main.cpp off this screen's own draft/mode (the library idiom): Load →
// the profile library in pick mode (NAV_SETUP_PICK); Edit → the editor on the run draft as a
// working copy (NAV_SETUP_EDIT, tweaks apply to THIS run only); Save-as → the editor to persist the
// draft as a named profile (NAV_SETUP_SAVE_AS); Start → Confirm (NAV_SETUP_START, C6b). Start is
// gated on readiness (a hard-valid compile + a healthy link; door-open joins in C7/PR5).
//
// LVGL-only; compiles for firmware and the native_ui_cyd / native_sim host targets.
#pragma once

#include <lvgl.h>

#include "oven_cal.h"
#include "profile_draft.h"

class SetupScreen {
public:
  enum class Page { Empty, Loaded };

  // Start a FRESH setup session for `mode` (Home → UV Cure / Reflow): clears the run draft and
  // lands on the Empty page. Call before showing the screen; render() then builds the current page.
  void enterMode(RecipeMode mode, const OvenModel &model = oven_cal::kDefaultModel);

  // Build the current page under `parent` (the router build cb). Call after enterMode()/setDraft(),
  // after lv_init() + ui_subjects_init().
  void render(lv_obj_t *parent);

  // Adopt a run draft chosen elsewhere (the picker's "Use this profile", or the editor working copy
  // on a committed tweak / Save-as): copies it in and marks the session Loaded. render() rebuilds.
  void setDraft(const ProfileDraft &draft);

  const ProfileDraft &draft() const { return draft_; }
  bool hasDraft() const { return have_draft_; }
  RecipeMode mode() const { return mode_; }

  // Can this run be started right now? A hard-valid compile (recipe_compiler) AND a healthy
  // controller link (subj_link_state). Door-open is added in C7/PR5 once the controller reports it.
  bool ready() const;

  // Exit seam: fired on Back (Home is the caller's to rebuild).
  void setExitHandler(void (*cb)(void *user_data), void *user_data);

  // Navigation + button targets (also directly callable by tests).
  void onLoad();   // → picker, pick mode (NAV_SETUP_PICK)
  void onEdit();   // → editor working copy of the run draft (NAV_SETUP_EDIT)
  void onSaveAs(); // → editor, persist the draft as a named profile (NAV_SETUP_SAVE_AS)
  void onStart();  // → Confirm (NAV_SETUP_START, C6b)
  void back();     // → exit handler (Home)

  Page page() const { return page_; }

private:
  void buildEmpty();
  void buildLoaded();
  void buildHeader(const char *title);
  void configParent();
  void clearParent();
  void publishNav(int nav_request);

  // The mode's editor ceiling → recipe_compiler Caps, for the readiness compile (the editor idiom).
  bool compileValid() const;

  lv_obj_t *parent_ = nullptr;
  const OvenModel *model_ = &oven_cal::kDefaultModel;
  RecipeMode mode_ = RecipeMode::Reflow;
  ProfileDraft draft_{};
  bool have_draft_ = false;
  Page page_ = Page::Empty;

  // Formatted strings held as members so the borrowed pointers the labels hold stay valid while the
  // page is shown (the SettingsScreen *_value_[] discipline).
  char facts_buf_[48]{};
  char prov_buf_[kProfileNameCap + 12]{};

  void (*on_exit_)(void *) = nullptr;
  void *exit_ud_ = nullptr;

  friend struct SetupThunks; // captureless thunks reach the private navigation methods
};
