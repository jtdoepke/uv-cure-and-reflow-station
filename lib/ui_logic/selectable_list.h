// Selectable list (§23 profile library / §24 settings hub) — the glove-safe ▲/▼-highlight + Open
// list pattern the design mandates so no screen relies on precise small-target taps. A scrolling
// column of full-width rows (label + optional right-aligned value) plus a footer of three large
// buttons: move the highlight Up / Down, and Open the highlighted row. Rows may also be tapped to
// highlight them directly; disabled rows ("coming soon" categories) are skipped by Up/Down and
// can't be opened.
//
// Same reusable shape as the numeric editors: the caller owns a SelectableListModel (holds the
// item list, the selected-index subject, and the Open seam), passes it to create_selectable_list,
// and the view binds to it. Pure navigation state — it never persists or energizes anything.
//
// LVGL-only; compiles for firmware and the native_ui_cyd / native_sim host targets.
#pragma once

#include <lvgl.h>

// One row's content. `value` (right-aligned, e.g. "100 °C") is optional; `enabled == false`
// renders the row greyed and excludes it from selection/Open. `verb` labels the footer action
// button while this row is highlighted (e.g. "Edit", "Toggle", "Change"); nullptr → "Open". So a
// list can mix actions — navigate one row, edit another — and the button always names what Open
// does. Strings are borrowed literals.
struct SelectableListItem {
  const char *label;
  const char *value;
  bool enabled;
  const char *verb = nullptr;
};

class SelectableListModel {
public:
  // Sized to hold a full profile library (§23): a ProfileList reply carries up to 32 rows
  // (oven.options, == the controller store's kMaxListed == ProfileLibraryViewModel::kMaxRows), and
  // the library binds the whole list to one model so ▲/▼ can walk all of them. The settings hub
  // uses a handful. Deliberately a plain literal rather than a reference to any of those: this
  // header stays free of the app_logic/protobuf include that a store or wire dependency would drag
  // in. A profile_library_screen static_assert ties the two together where both are already
  // visible.
  static constexpr int kMaxItems = 32;

  // Prepare the model for a fresh list. Call after lv_init() and before building the view. Copies
  // up to kMaxItems items (the structs, not the borrowed strings) and selects the first enabled
  // row. `wrap == true` makes Up/Down loop around the ends (last→first, first→last) instead of
  // saturating, and keeps both footer buttons enabled whenever ≥2 rows are selectable.
  // Re-init-safe; clears the Open seam (re-install it after init()).
  void init(const SelectableListItem *items, int count, bool wrap = false);

  // Footer intents: move the highlight to the previous / next *enabled* row. Saturating at the ends
  // unless the model was init()'d with wrap == true, in which case they loop around.
  void moveUp();
  void moveDown();

  // Highlight a specific row (row-tap seam); ignored if the index is out of range or disabled.
  void select(int index);

  // Open the highlighted row: fires the Open handler with the selected index (no-op if that row
  // is somehow disabled). Neither this nor selection persists or energizes anything.
  void onOpen();

  void setOpenHandler(void (*cb)(int index, void *user_data), void *user_data);

  int selected() { return lv_subject_get_int(&selected_subject_); }
  int count() const { return count_; }
  const SelectableListItem &item(int i) const { return items_[i]; }
  bool atFirstEnabled(); // true → the Up button disables
  bool atLastEnabled();  // true → the Down button disables
  bool canOpen();        // false → the Open button disables (empty list / no selectable row)
  lv_subject_t *selectedSubject() { return &selected_subject_; }

private:
  int firstEnabled() const;
  int lastEnabled() const;
  int nextEnabled(int from) const; // next enabled index > from, or `from` if none
  int prevEnabled(int from) const; // prev enabled index < from, or `from` if none

  SelectableListItem items_[kMaxItems]{};
  int count_ = 0;
  bool wrap_ = false;
  lv_subject_t selected_subject_{};
  void (*on_open_)(int, void *) = nullptr;
  void *open_ud_ = nullptr;
};

// An optional footer button to the LEFT of ▲/▼/Open — the §23 profile library's `+ New`, which the
// settings hub has no use for. Its click is a plain event (its own user_data), independent of the
// model's selection, so it can trigger a create/exit action rather than acting on the highlight.
struct LeadingAction {
  const char *label = nullptr;      // nullptr → no leading button (the plain 3-button footer)
  lv_event_cb_t on_click = nullptr; // fired on LV_EVENT_CLICKED
  void *user_data = nullptr;
};

// Handles into the built list so callers/tests can inspect or drive widgets.
struct SelectableList {
  lv_obj_t *root;
  lv_obj_t *list; // the scrolling row container
  lv_obj_t *rows[SelectableListModel::kMaxItems];
  lv_obj_t *btn_leading; // the optional leading action (nullptr when none)
  lv_obj_t *btn_up;
  lv_obj_t *btn_down;
  lv_obj_t *btn_open;
};

// Build the list + footer under `parent`. `model` must be init()'d first and must outlive the
// returned widgets (they bind its subject and hold it as callback user_data). `leading`, when its
// label is non-null, prepends a fourth footer button (e.g. `+ New`).
SelectableList create_selectable_list(lv_obj_t *parent, SelectableListModel &model,
                                      const LeadingAction &leading = {});
