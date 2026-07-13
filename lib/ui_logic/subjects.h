// subjects — the shared UI state, and the *only* globals in the UI layer (LVGL Observer
// pattern: subjects are the sole interface between the view and the app logic). Views bind to
// them; view models write them; whoever owns real data (idle telemetry decode, the §9
// handshake) writes them too. Declared extern here, defined once in subjects.cpp.
//
// LVGL-only; compiles for firmware and the native test/sim targets.
#pragma once

#include <lvgl.h>

// Machine state as shown on Home (§14). RUNNING/FAULT are carried for other screens; Home only
// distinguishes idle-and-cool from HOT for its safety band + sleep suppression (§17).
enum RunState {
  RUN_IDLE = 0,
  RUN_HOT = 1,
  RUN_RUNNING = 2,
  RUN_FAULT = 3,
};

// Controller link health (§9). Anything but LINK_OK gates the run flow: mode buttons disable
// and Home shows the "controller not responding" banner.
enum LinkState {
  LINK_OK = 0,
  LINK_NONE = 1,   // no telemetry / heartbeat
  LINK_SCHEMA = 2, // handshake schema-hash mismatch
};

// Placeholder navigation intent. Home publishes one of these on a tile tap; a real screen
// manager consumes it when the destination screens land (C4/C6). Until then it is observed
// only by tests.
enum NavRequest {
  NAV_NONE = 0,
  NAV_CURE_SETUP,
  NAV_REFLOW_SETUP,
  NAV_PROFILES,
  NAV_CALIBRATE,
  NAV_SETTINGS,
};

extern lv_subject_t subj_chamber_temp; // int, °C
extern lv_subject_t subj_run_state;    // RunState
extern lv_subject_t subj_link_state;   // LinkState
extern lv_subject_t subj_nav_request;  // NavRequest

// (Re)initialise every subject to a safe boot default. Call once after lv_init() and before
// building any screen — the firmware setup(), the sim, and each test's setUp() all do this.
// Idempotent: re-running it resets the shared state to defaults, which is exactly what the
// per-test setUp() wants after lv_deinit().
void ui_subjects_init();
