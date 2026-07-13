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
constexpr int32_t UV_CAP_DEFAULT = 100;
constexpr int32_t REFLOW_CAP_DEFAULT = 250;
constexpr int32_t IDLE_TIMEOUT_DEFAULT_MIN = 2;
constexpr int32_t BRIGHTNESS_BIAS_DEFAULT = 0;

// Per-mode caution shown when a cap is raised above its default (design.md:2237-2244). The
// wording is honest per mode: the L0 hardware fuse sits at reflow level, so at cure/UV temps
// only firmware + high-limit protect — do not claim the fuse governs there. ASCII only (the UI
// font carries 0x20-0x7F + ° only; no em-dash).
constexpr const char *UV_CAP_CAUTION =
    "Above default - protection is firmware + high-limit only at these temperatures";
constexpr const char *REFLOW_CAP_CAUTION =
    "Above default - thermal fuse still governs at reflow temperatures";

} // namespace settings_defaults
