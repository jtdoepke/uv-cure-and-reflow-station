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

#include "management_client.h"
#include "oven_cal.h"
#include "profile_draft.h" // ProfileDraft — the run working copy pick mode hands back (C6)
#include "profile_library_viewmodel.h"
#include "selectable_list.h"

class ProfileLibraryScreen {
public:
  // Loading/Error are the async states the remote store added (Wave R3b): the list/detail/actions
  // are now round-trips to the controller (§9), so the screen shows a spinner while a reply is in
  // flight and an error if it times out or is refused.
  enum class Page { Chooser, List, Detail, ConfirmDelete, Rename, Loading, Error };

  // Build the chooser under `parent`, over the shared remote ManagementClient (§9; the profile
  // library lives on the controller now). Both must outlive this screen. Call after lv_init() +
  // ui_subjects_init(). `model` defaults to the compiled-in calibration; a test may pass a toy one.
  void begin(lv_obj_t *parent, ManagementClient &client,
             const OvenModel &model = oven_cal::kDefaultModel);

  // Enter PICK mode for the Setup screen's "Load a profile" (§19/C6): skip the mode chooser (Setup
  // already knows the mode), open that mode's list ordered most-recently-used by default with a
  // sort toggle, and on a profile's "Use this profile" hand the assembled run ProfileDraft to the
  // pick handler instead of opening the editor. Back exits to Setup. Management verbs are hidden —
  // pick mode is read-only selection, never a mutation. `parent`/`client`/`model` must outlive
  // this.
  void beginPick(lv_obj_t *parent, ManagementClient &client, RecipeMode mode,
                 const OvenModel &model = oven_cal::kDefaultModel);
  // Pick handler: fired with the chosen profile's run working copy (§19/C6). Set before beginPick.
  void setPickHandler(void (*cb)(void *user_data, const ProfileDraft &draft), void *user_data);

  // Drive the async state machine: call every loop iteration (after client.service()). Consumes a
  // Ready/Failed reply for this screen's outstanding request and rebuilds the page. A no-op unless
  // this screen is waiting on a reply.
  void poll();

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
  void onRenameRequested();              // detail → the name-entry keyboard (user profiles only)
  void onRenameCommit(const char *text); // ✓ on the keyboard → rename in the store, back to list
  void onDeleteRequested();              // → the confirm dialog
  void onDeleteConfirmed();              // Delete, then back to the list

  // Pick-mode actions (§19/C6).
  void onPickUse();           // detail "Use this profile" → hand the run draft to the pick handler
  void toggleSortAndReload(); // list sort toggle → flip MRU⇄alpha and re-fetch the list

  // ▲/▼ pressed at a loaded end of a paged window (§23): fetch the adjacent one. `dir` -1 = above,
  // +1 = below. The highlight lands on the abutting row so scrolling reads as continuous.
  void onPageEdge(int dir);

  // Record the profile the next adopted list should highlight, by NAME, and request it as the
  // anchor. Used by every mutation, since a row's index is not stable across one.
  void setAnchor(const char *name);

  bool pickMode() const { return pick_; }

  // Inspection (tests).
  Page page() const { return page_; }
  RecipeMode mode() const { return mode_; }
  int selected() const { return selected_; }
  SelectableListModel &listModel() { return list_model_; }
  ProfileLibraryViewModel &vm() { return *current_; }

private:
  // What this screen's outstanding request is for — poll() dispatches on it when the reply lands.
  enum class Pending { None, List, Detail, Action };

  void buildChooser();
  void buildList();
  void buildDetail();
  void buildConfirm();
  void buildRename(); // the shared name-entry keyboard, prefilled with the current name
  void buildLoading(const char *msg);  // centred spinner label while a reply is in flight
  void buildError();                   // "couldn't reach the controller" + Back
  void buildHeader(const char *title); // < Back + title into `parent_`
  void configParent();
  void clearParent();
  void publishNav(int nav_request); // set subj_nav_request (the deferred-destination seam)

  lv_obj_t *parent_ = nullptr;
  Page page_ = Page::Chooser;
  RecipeMode mode_ = RecipeMode::Reflow;
  int selected_ = 0; // remembered highlight (WINDOW-relative), restored list → (detail →) list

  // How the next adopted list should place the highlight (§23 paging). The window is a moving view
  // over the library, so "row 3" is not a stable identity across a refresh — these say what the
  // highlight actually meant.
  enum class Restore {
    Index, // keep selected_ (a plain refresh of the same window)
    Name,  // put it on pending_anchor_, wherever that row landed (after a mutation)
    First, // top row (paged down into a new window)
    Last,  // bottom row (paged up into a new window)
  };
  Restore restore_ = Restore::Index;
  // The row a mutation acted on, sent as ProfileListReq.anchor_name so the controller returns
  // whichever window now holds it. Empty when restore_ != Restore::Name.
  char pending_anchor_[kProfileNameCap] = {};

  ManagementClient *client_ = nullptr;
  Pending pending_ = Pending::None;
  Page return_page_ = Page::Chooser; // where an error's Back goes (the last stable page)

  // Pick mode (§19/C6): the Setup screen's "Load a profile". Read-only selection — no chooser, no
  // management verbs; a chosen profile's run draft goes to on_pick_ instead of the editor.
  bool pick_ = false;
  void (*on_pick_)(void *, const ProfileDraft &) = nullptr;
  void *pick_ud_ = nullptr;

  // One view-model, re-init'd with the mode on openMode (only one mode's library is ever shown —
  // two full caches wasted ~3 KB of the scarce DRAM). current_ keeps the existing *current_ call
  // sites working. model_ is stored so openMode can re-init.
  const OvenModel *model_ = nullptr;
  ProfileLibraryViewModel vm_;
  ProfileLibraryViewModel *current_ = &vm_;
  SelectableListModel list_model_; // the mode-scoped list (the chooser is two tiles)
  lv_obj_t *rename_ta_ = nullptr;  // the Rename page's textarea (read on the ✓ key)

  void (*on_exit_)(void *) = nullptr;
  void *exit_ud_ = nullptr;

  friend struct ProfileThunks; // grants the .cpp's captureless thunks access to private methods
};
