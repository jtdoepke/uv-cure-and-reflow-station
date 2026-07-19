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

// Navigation intent. A screen publishes one on a tap; a subject-level observer in main.cpp (which
// survives screen swaps) routes it. Home drives NAV_PROFILES/NAV_SETTINGS today; the profile
// library (C4) drives the NAV_PROFILE_* pair below (routed to the editor, C5).
enum NavRequest {
  NAV_NONE = 0,
  NAV_CURE_SETUP,
  NAV_REFLOW_SETUP,
  NAV_PROFILES,
  NAV_CALIBRATE,
  NAV_SETTINGS,
  // Profile-library actions that open the editor (§12/C5). The *which profile* + editable
  // working-copy handoff is resolved in main.cpp off the library's own selection state. NEW →
  // editor on a fresh template; EDIT → editor on the selected profile. There is deliberately no
  // LOAD intent from the Profiles branch: running a profile is a separate path from Home (UV Cure /
  // Reflow → Setup, §19), which is where LOAD lives (NAV_SETUP_* below).
  NAV_PROFILE_NEW,
  NAV_PROFILE_EDIT,
};

extern lv_subject_t subj_chamber_temp; // int, °C
extern lv_subject_t subj_run_state;    // RunState
extern lv_subject_t subj_link_state;   // LinkState
extern lv_subject_t subj_nav_request;  // NavRequest

// Device-settings state with cross-screen consumers (§24). The temp caps publish here so the
// profile editor (§12) can read them as editor ceilings "with no extra wiring"; units applies
// everywhere (editor/run/about); Advanced gates advanced profile editing. Settings-local
// preferences (auto-brightness, idle timeout, brightness bias) stay inside the Settings screen.
// Written by the Settings screen from the SettingsStore; read by whoever needs them.
extern lv_subject_t subj_units;      // 0 = °C, 1 = °F
extern lv_subject_t subj_uv_cap;     // int, °C — UV/cure user max-temp cap
extern lv_subject_t subj_reflow_cap; // int, °C — reflow user max-temp cap
extern lv_subject_t subj_advanced;   // 0/1 — Advanced options unlocked

// A hardware capability, not a preference: whether this board has an ambient-light sensor fitted
// (the 3.5" panel does not). Published once at boot by the firmware from cyd_board.h; the Settings
// screen greys out the auto-brightness row when it is 0. It arrives as data precisely so that
// lib/ui_logic never learns what a board is — a #if here would be a board identity leaking into
// host-testable code, which is the thing the ports exist to prevent. Defaults to 1 so the sim and
// the UI tests exercise the fitted case unless a test says otherwise.
extern lv_subject_t subj_has_ambient_light; // 0/1 — ambient-light sensor present

// (Re)initialise every subject to a safe boot default. Call once after lv_init() and before
// building any screen — the firmware setup(), the sim, and each test's setUp() all do this.
// Idempotent: re-running it resets the shared state to defaults, which is exactly what the
// per-test setUp() wants after lv_deinit().
void ui_subjects_init();
