// Value-stepper editor (§24, C2) — the shared, glove-sized, one-value-per-screen numeric editor
// for nudge-range fields (≤20 steps: idle timeout, brightness bias). Header (field name), a
// centred [ − ] value+units [ + ] row with large (~96 px) buttons that disable at min/max, a
// "Range lo–hi · default N" context line, an amber caution line (raising above default), and a
// Cancel / Save footer. Tapping the value opens the keypad (§26, C1) for direct entry.
//
// It is the first reusable, per-instance widget: the caller owns a ValueStepperViewModel (which
// holds the working value + field config) and passes it in, mirroring how create_home_screen
// takes the caller-provided view model. Built under `parent`; the hosting screen (Settings §24,
// profile editor §12) supplies global chrome.
//
// LVGL-only; compiles for firmware and the native_ui_cyd / native_sim host targets.
#pragma once

#include <lvgl.h>

#include "value_stepper_viewmodel.h"

// Handles into the built editor so callers/tests can inspect or drive widgets.
struct ValueStepper {
  lv_obj_t *root;
  lv_obj_t *title_label;   // field name being edited
  lv_obj_t *btn_minus;     // large − (disabled at min)
  lv_obj_t *value_label;   // big centred "<value> <units>" (tap → keypad)
  lv_obj_t *btn_plus;      // large + (disabled at max)
  lv_obj_t *range_label;   // "Range lo–hi · default N"
  lv_obj_t *caution_label; // amber; hidden unless value > default
  lv_obj_t *btn_cancel;    // discard, restore opening value
  lv_obj_t *btn_save;      // commit current value
};

// Build the editor under `parent` (e.g. lv_screen_active()) for the field `vm` describes.
// `vm` must be init()'d first and must outlive the returned widgets (they bind to its subject
// and hold it as callback user_data). `title` is the field name shown in the header.
ValueStepper create_value_stepper(lv_obj_t *parent, ValueStepperViewModel &vm, const char *title);
