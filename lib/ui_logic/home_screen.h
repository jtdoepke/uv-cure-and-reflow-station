// Home/Status hub (§14) — root of the hub-and-spoke UI and the screen that sets the visual
// language for the rest (C4/C7/C8 build on it). Its job: pick what to do while making the
// machine's safety state unmissable. Header (title + link glyph+word), status band (state
// badge + live chamber temp), two big mode tiles (UV CURE / REFLOW), and a secondary row
// (Profiles / Calibrate / Settings). No Back and no STOP — Home is one of the two footer-rule
// exceptions (§13): it is the idle root, unreachable during a run.
//
// The screen only binds to the shared subjects (subjects.h). Who feeds them — idle telemetry
// decode + the §9 handshake — is wired separately; the sim and tests set them directly to
// exercise every state.
//
// LVGL-only; compiles for firmware and the native_ui_cyd / native_sim host targets.
#pragma once

#include <lvgl.h>

// Handles into the built screen so callers/tests can inspect or drive widgets.
struct HomeScreen {
  lv_obj_t *root;
  lv_obj_t *link_label;    // header link indicator (glyph + word)
  lv_obj_t *state_dot;     // status-band colour dot (redundant with the word)
  lv_obj_t *state_label;   // status-band machine-state word
  lv_obj_t *chamber_label; // "Chamber NN °C"
  lv_obj_t *banner;        // "controller not responding" — hidden when link OK
  lv_obj_t *btn_cure;      // UV CURE mode tile
  lv_obj_t *btn_reflow;    // REFLOW mode tile
  lv_obj_t *btn_profiles;
  lv_obj_t *btn_calibrate;
  lv_obj_t *btn_settings;
};

// Build the Home screen under `parent` (e.g. lv_screen_active()). Requires ui_subjects_init()
// to have run first.
HomeScreen create_home_screen(lv_obj_t *parent);
