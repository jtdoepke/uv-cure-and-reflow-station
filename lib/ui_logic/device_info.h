// device_info — the handful of identity strings the About panel (§24) reports, injected by the
// firmware rather than known by the UI.
//
// About exists to answer "what am I actually running?", so a hard-coded answer there is worse than
// no answer: it stayed "ESP32-2432S028" for the whole life of the 3.5" board and read as fact.
// But lib/ui_logic must never learn a board identity (that is cyd_board.h's job, and only the
// firmware may include it), and the board name is a *string*, so the lv_subject_t int channel the
// other capabilities ride on does not fit.
//
// Hence a plain injected struct: src_cyd/main.cpp calls ui_set_device_info() at boot with values
// from cyd_board.h + lib/protocol, and About reads it back. The strings are borrowed literals with
// static lifetime (firmware-owned, never freed) — the same contract NumericFieldConfig::units has.
// Anything not set keeps a default that says so, out loud, rather than guessing.
#pragma once

struct DeviceInfo {
  const char *board = "unknown"; // e.g. "ESP32-3248S035 (3.5\" ST7796S)"
  const char *firmware = "dev";  // build identity
  const char *panel = "unknown"; // e.g. "320x480 portrait"
};

// Set at boot, before the Settings screen can be opened. Copies the (borrowed) pointers only.
void ui_set_device_info(const DeviceInfo &info);

// The current values; defaults until ui_set_device_info() is called (the sim + host tests never
// call it, and "unknown" is the honest thing for them to show).
const DeviceInfo &ui_device_info();
