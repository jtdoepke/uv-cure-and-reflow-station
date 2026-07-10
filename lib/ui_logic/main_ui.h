// main_ui — builds the demo UI (title + tap-counting button) as a testable unit.
//
// LVGL-only: no <Arduino.h>, no LovyanGFX. That lets it compile for both the firmware and
// the native_ui host test env, where LVGL's headless dummy display + simulated input drive
// it and assert the label updates (see test/test_ui/). src/main.cpp just calls
// create_main_ui() after the display is up.
#pragma once

#include <lvgl.h>

// Handles into the created UI, so callers/tests can inspect or update widgets.
struct MainUi {
  lv_obj_t *title;     // "Hello CYD!"
  lv_obj_t *button;    // "Tap me"
  lv_obj_t *btn_label; // shows "Tap me" / "Touched N"
};

// Build the demo UI under `parent` (e.g. lv_screen_active()). The button's click handler
// increments a counter and rewrites btn_label to "Touched N".
MainUi create_main_ui(lv_obj_t *parent);
