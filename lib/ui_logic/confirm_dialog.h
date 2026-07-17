// confirm_dialog — a simple modal yes/no confirm (design.md §23).
//
// A scrim over the screen plus a centred panel: a message and two glove-sized buttons, Cancel
// (default / safe) and a caller-named confirm verb. Used for the profile library's Delete — a
// *simple* confirm, deliberately NOT the press-and-hold arm-then-start gesture, which §19/§22
// reserve for energizing hazards (heat/UV). Deleting a saved file is destructive but not an
// energizing hazard, so danger-red is likewise NOT used here (§13/§22 reserve it for the hazardous
// verb) — the confirm step itself is the guard.
//
// Captureless seams (the codebase's single-void*-user_data idiom). The dialog does not delete
// itself: its handlers navigate, and the owning screen's rebuild (lv_obj_clean) removes it — the
// same lifecycle SettingsScreen's editors use.
//
// LVGL-only; compiles for firmware and the native_ui_cyd / native_sim host targets.
#pragma once

#include <lvgl.h>

struct ConfirmDialog {
  lv_obj_t *root;  // the full-screen scrim (top of the z-order, blocks the page beneath)
  lv_obj_t *panel; // the centred dialog box
  lv_obj_t *btn_confirm;
  lv_obj_t *btn_cancel;
};

// Build a modal confirm over `parent` (typically the current page container). `message` is shown
// wrapped; `confirm_label` names the confirming action (e.g. "Delete"). `on_confirm` / `on_cancel`
// fire with `user_data`; either may be nullptr.
ConfirmDialog create_confirm_dialog(lv_obj_t *parent, const char *message,
                                    const char *confirm_label, void (*on_confirm)(void *user_data),
                                    void (*on_cancel)(void *user_data), void *user_data);
