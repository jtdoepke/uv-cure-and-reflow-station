// profile_editor_screen — the §12 profile editor screen pair (backlog C5).
//
// A self-contained hub-and-spoke controller (the SettingsScreen / ProfileLibraryScreen pattern): it
// owns a `parent` and rebuilds it as the user moves Overview → Phase editor → field keypad → name
// entry, create-on-demand (no PSRAM). Edits parameters, never the curve (§12 "edit parameters, not
// the curve"): each phase's numbers go through the shared constrained keypad, a read-only
// feasibility curve is derived from them, and Save commits the whole profile to a ProfileStore.
//
// The editor works on a WORKING COPY: beginEdit() takes the profile to edit (a library profile from
// C4's Edit, later a run's working copy from C6's Setup) plus the target store; every edit mutates
// the internal copy, and only Save writes it back — leaving discards. Editing a stock profile (or a
// fresh New) is a Save-as: the first Save routes through name entry (§23). Validation is the shared
// recipe_compiler's (recipe_compiler.h): Save is gated on hardValid (an accepted compile always
// uploads, §9) and an amber advisory is shown for a physically-optimistic profile — never a second
// validator.
//
// The two per-mode ProfileStores + the library screen are wired to this in main.cpp: NAV_PROFILE_
// NEW/EDIT seed the working copy and route here; the exit seam returns to the library. Pure MVVM:
// selection state lives in the SelectableListModel's lv_subject_t; the pure fact/curve math is
// profile_facts.h / recipe_compiler.h. LVGL-only; compiles for firmware + native_ui_cyd /
// native_sim.
#pragma once

#include <lvgl.h>

#include "numeric_keypad_viewmodel.h"
#include "oven_cal.h"
#include "profile_store.h"
#include "recipe_compiler.h" // compileRecipe / CompileResult / Caps (validation, §12)
#include "selectable_list.h"
#include "value_stepper_viewmodel.h"

class ProfileEditorScreen {
public:
  enum class Page { Overview, PhaseEditor, FieldEditor, NameEntry };

  // Set the profile to edit + where Save writes it. `working` is copied into an internal buffer
  // (the caller's is untouched until Save). `saveAs` forces name entry on the first Save (a fresh
  // New, or a stock source whose factory reference must survive, §23). `model` supplies the
  // calibration the feasibility preview/validation run against (default = the compiled-in
  // constants; tests pass a toy model). Does not touch LVGL — render() builds the UI.
  void beginEdit(const ProfileStore::StoredProfile &working, ProfileStore &target, bool saveAs,
                 const OvenModel &model = oven_cal::kDefaultModel);

  // Build the current page under `parent` (the router build cb; call after beginEdit(), after
  // lv_init() + ui_subjects_init()).
  void render(lv_obj_t *parent);

  // Exit seam: fired when the editor leaves (Back off the Overview, or a completed Save). The
  // caller rebuilds the library.
  void setExitHandler(void (*cb)(void *user_data), void *user_data);

  // Navigation + button/seam targets.
  void showPage(Page page);
  void back();
  void openPhase(int index); // Overview row → that phase's field editor
  void
  onFieldOpen(int index); // Phase-editor row → keypad (numeric) / cycle (fan) / toggle (uv/motor)
  void onSave();          // Overview Save → name entry if needed, else commit + exit
  void commitName(const char *name); // Name-entry OK: adopt a valid name, then Save (§23 Save-as)

  // Advanced structure edits (§12; the UI only shows these when subj_advanced, but the methods are
  // always callable so tests can drive them directly). Each mutates the working copy +
  // re-validates.
  void addPhase();      // append a blank phase (+ Add)
  void deletePhase();   // remove the highlighted phase (never below one)
  void movePhaseUp();   // reorder: swap the highlighted phase with the one above
  void movePhaseDown(); // reorder: swap with the one below

  // Inspection (tests).
  Page page() const { return page_; }
  RecipeMode mode() const { return mode_; }
  int selectedPhase() const { return selected_phase_; }
  const ProfileStore::StoredProfile &working() const { return working_; }
  bool hardValid() const { return validation_.hardValid; }
  bool hasAmber() const { return validation_.hasAmber(); }
  bool savedOk() const { return saved_ok_; } // last onSave() committed to the store
  SelectableListModel &listModel() { return list_model_; }
  NumericKeypadViewModel &keypadVm() { return keypad_vm_; }
  ValueStepperViewModel &stepperVm() { return stepper_vm_; }

private:
  // Which numeric field a keypad edit targets (the phase-editor's editable numbers).
  enum class NumField { None, Target, Ramp, Hold };
  // What a phase-editor row does. The row SET differs by mode (cure adds UV/motor), so a built-row
  // → action map identifies the action rather than the raw index (the SettingsScreen DisplayRow
  // idiom).
  enum class FieldRow { Target, Ramp, Hold, ConvFan, Uv, Motor };

  // Page builders (each clears `parent_` and lays out its own content).
  void buildOverview();
  void buildPhaseEditor();
  void buildNameEntry();
  void buildHeader(const char *title); // ‹ Back + title into parent_
  void configParent();
  void clearParent();

  // Overview helpers.
  void rebuildOverviewRows();         // (re)fill phase_label_/phase_value_ + the list model
  const char *validationWord() const; // the red/amber banner word for validation_ (nullptr = clean)

  // Phase-editor field helpers.
  void openField(NumField field);
  void commitField(int32_t value);
  static FanMode cycleFan(FanMode m); // Auto → On → Off → Auto (cycle-on-Open, §12)
  NumericFieldConfig fieldConfig(NumField field) const;
  bool holdIsExposure(const Phase &p) const; // cure + calibrated + uv + motor → dose authoring
  const char *holdRowLabel(const Phase &p) const;
  const char *holdNote(const Phase &p) const; // amber inline note, or nullptr (§12)

  // Shared.
  Caps caps() const;
  Phase &currentPhase();
  const Phase &currentPhase() const;
  void recompute(); // refresh validation_ from the working copy
  void doSave();    // write the working copy to target_ and exit

  lv_obj_t *parent_ = nullptr;
  Page page_ = Page::Overview;

  ProfileStore::StoredProfile working_{};
  ProfileStore *target_ = nullptr;
  const OvenModel *model_ = &oven_cal::kDefaultModel;
  bool save_as_ = false;
  bool saved_ok_ = false;
  RecipeMode mode_ = RecipeMode::Reflow;
  int selected_phase_ = 0; // highlighted phase (Overview), restored returning from the phase editor
  int field_sel_ = 0; // highlighted field row (Phase editor), restored returning from the keypad

  NumField editing_field_ = NumField::None;
  CompileResult validation_{};

  // Reused models/VMs — members so their subjects survive each page rebuild.
  SelectableListModel list_model_;
  ValueStepperViewModel stepper_vm_;
  NumericKeypadViewModel keypad_vm_;

  // Row index → action for the currently built phase-editor field list.
  FieldRow field_rows_[6]{};
  int field_row_count_ = 0;

  // Formatted strings — members so the borrowed pointers the list rows hold stay valid while a page
  // is shown (the SettingsScreen *_value_[] discipline).
  char phase_label_[kMaxPhases][24]{};
  char phase_value_[kMaxPhases][24]{};
  char field_value_[6][24]{};
  char hold_label_[16]{};
  char header_buf_[32]{};

  // Name-entry (the free-text exception, §12/§26).
  lv_obj_t *name_ta_ = nullptr;
  char name_buf_[kProfileNameCap]{};

  void (*on_exit_)(void *) = nullptr;
  void *exit_ud_ = nullptr;

  friend struct EditorThunks; // captureless thunks reach the private navigation/commit methods
};
