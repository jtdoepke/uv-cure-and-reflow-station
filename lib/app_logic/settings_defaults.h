// settings_defaults.h — CYD-side constants for the device settings (design.md §4, §24).
//
// These are the *policy* numbers the CYD enforces at the editor + pre-send validation (§4
// "Enforcement"): the per-mode absolute hard-max that bounds how high a user cap can go, the
// factory defaults the firmware ships, and the per-mode caution wording. The controller keeps
// its OWN reviewed hard-max header and enforces it independently (untrusted-CYD-proof, §4/§9) —
// nothing here weakens that; a stale/too-high value on this side is still caught downstream.
//
// Pure constants: no LVGL, no Arduino. Consumed by settings_store.h (bounds + boot clamp) and,
// through the NumericFieldConfig factories there, by the settings UI editors.
#pragma once

#include <cstdint>

namespace settings_defaults {

// Absolute per-mode hard-max (§4 layer 1) — the ceiling a user cap can never loosen past, and
// the value stored caps are clamped down to at boot (§7/§24). Reflow = 300 °C is DECIDED
// (design.md:175).
constexpr int32_t REFLOW_HARD_MAX = 300;
// UV/cure hard-max is still an OPEN question (§10, design.md:176 "value TBD") — keep it a
// conservative fixed ceiling here until it is decided and a matching reviewed constant lands in
// the controller tree (A4b/A7). Revisit both together.
constexpr int32_t UV_HARD_MAX = 120;

// Shared lower bound for both temp-cap editors (a cap below this is not useful).
constexpr int32_t TEMP_CAP_MIN = 60;

// Factory defaults the firmware ships (design.md:2251 / §4). A first boot / "restore defaults"
// lands here.
// 110 rather than 100 since 2026-07-22: the stock cure set's Flame Retardant profile post-cures
// at 100 °C (Form Cure V2), which at a 100 °C default compiled with *zero* margin — the compiler
// rejects on `targetC > capC`, so any operator who trimmed their cap at all would break a factory
// profile. This is the shipped default of the user setting only; the layer-1 absolute hard-max
// (UV_HARD_MAX / oven_safety::CURE_HARD_MAX_C) stays 120, and 110 sits inside it, so the §4
// ceiling and the boot clamp are untouched. Mirrored on the controller — control::defaultSettings()
// in lib/control_logic/device_settings.h holds its own copy (§4: the controller never trusts the
// CYD's numbers), and the two must move together or a fresh CYD and a fresh controller disagree
// about what shipped.
constexpr int32_t UV_CAP_DEFAULT = 110;
constexpr int32_t REFLOW_CAP_DEFAULT = 250;
constexpr int32_t IDLE_TIMEOUT_DEFAULT_MIN = 2;
constexpr int32_t BRIGHTNESS_BIAS_DEFAULT = 0;

// Screen brightness (§18), as a percent of full scale. This is the setting a board with NO light
// sensor offers *instead of* the brightness bias: a bias is a trim on an ambient reading, so with
// nothing to read it is a trim on a constant — an indirection with no second term. The board's
// capability picks which row appears, as data (subj_has_ambient_light), never as an #if in the UI.
//
// The 20% floor is the load-bearing part. It exists so the control can always be found again: set
// the screen black and the only way back is a reflash. It is deliberately pitched ABOVE
// AutoBrightness::Config::floorLevel (48/255 = 19%), the non-defeatable safety minimum that keeps
// HOT / UV ON / fault legible in a dark shop — 20% lands at level 51, so every step of this field
// actually moves the panel. Lower the floor here and the bottom of the range silently clamps
// against that safety floor instead: a dead control, which is exactly the failure the bias field's
// own comment already records. If floorLevel ever moves, re-check this number against it.
constexpr int32_t SCREEN_BRIGHTNESS_MIN_PCT = 20;
constexpr int32_t SCREEN_BRIGHTNESS_MAX_PCT = 100;
constexpr int32_t SCREEN_BRIGHTNESS_STEP_PCT = 10; // 8 steps -> the stepper, not the keypad
constexpr int32_t SCREEN_BRIGHTNESS_DEFAULT_PCT = 100;

// Per-mode caution shown when a cap is raised above its default (design.md:2237-2244). The
// wording is honest per mode: the L0 hardware fuse sits at reflow level, so at cure/UV temps
// only firmware + high-limit protect — do not claim the fuse governs there. ASCII only (the UI
// font carries 0x20-0x7F + ° only; no em-dash).
constexpr const char *UV_CAP_CAUTION =
    "Above default - protection is firmware + high-limit only at these temperatures";
constexpr const char *REFLOW_CAP_CAUTION =
    "Above default - thermal fuse still governs at reflow temperatures";

} // namespace settings_defaults
