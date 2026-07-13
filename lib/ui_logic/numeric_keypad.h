// On-screen numeric keypad (§26, C1) — the shared editor for every wide-range numeric field (the
// >20-step rule, §24): temp caps, phase target/ramp/hold, exposure. A familiar 3×4 digit grid
// (1-2-3 / 4-5-6 / 7-8-9 / ⌫-0-OK) filling a left 224 px zone, plus a right-hand rail carrying
// the field name, the live value+units, the always-shown range, an amber caution, and the ✕
// Cancel key. Integer-only — no decimal point, no sign (§26).
//
// Entry is constrained, not validated-after: an over-max digit is refused, OK is disabled until
// the value lands in [min,max], and the value shows amber with the range reminder until then. ⌫
// deletes the last digit; long-press ⌫ empties.
//
// Like the value-stepper (§24, C2), it is a per-instance widget: the caller owns a
// NumericKeypadViewModel (working value + field config) and passes it in. Built under `parent`;
// the hosting screen supplies global chrome, and real caller navigation is wired later (C5/C8).
//
// LVGL-only; compiles for firmware and the native_ui_cyd / native_sim host targets.
#pragma once

#include <lvgl.h>

#include "numeric_keypad_viewmodel.h"

// Handles into the built keypad so callers/tests can inspect or drive widgets. `keys[0..9]` are
// the digit buttons indexed by digit; the control keys are named.
struct NumericKeypad {
  lv_obj_t *root;
  lv_obj_t *keys[10];      // digit buttons, indexed by the digit they type
  lv_obj_t *btn_backspace; // ⌫ — click deletes a digit, long-press empties
  lv_obj_t *btn_ok;        // OK ✓ — disabled until the value is in range
  lv_obj_t *btn_cancel;    // ✕ — discard, return the old value
  lv_obj_t *title_label;   // field name being edited
  lv_obj_t *value_label;   // big "<value> <units>"; amber while OK is disabled
  lv_obj_t *range_label;   // "min-max units" (always shown)
  lv_obj_t *caution_label; // amber; hidden unless value > default
};

// Build the keypad under `parent` (e.g. lv_screen_active()) for the field `vm` describes. `vm`
// must be init()'d first and must outlive the returned widgets (they bind to its subjects and
// hold it as callback user_data). `title` is the field name shown in the rail.
NumericKeypad create_numeric_keypad(lv_obj_t *parent, NumericKeypadViewModel &vm,
                                    const char *title);
