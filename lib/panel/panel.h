// panel — the display's physical geometry as compile-time numbers (design.md §2).
//
// The one place the panel's native dimensions, mounting rotation and pixel pitch enter the
// build. lib/ui_logic reads it to size widgets in millimetres, include/cyd_board.h reads it to
// derive the framebuffer, sim/ and the native test envs read it to create a display of the right
// size. Deliberately dependency-free (<cstdint> only, its own lib like lib/calibration) so
// including it never drags in LVGL or Arduino.
//
// A board IDENTITY (CYD_BOARD_*) must never reach here: ui_logic compiles for native_ui_cyd and
// native_sim, where no board exists. Geometry is a property of the panel, not of the board that
// carries it, so it gets neutral PANEL_* names that both sides read — one source, no drift.
//
// Set per board by a [board_*] section in platformio.ini. The #ifndef defaults (the
// #ifndef-override idiom lv_conf.h uses for LV_USE_LODEPNG) mirror the default_envs board so a
// bare compile — clangd, a stray TU — still works.
#pragma once

#include <cstdint>

#ifndef PANEL_NATIVE_W
#define PANEL_NATIVE_W 240 // 2.8" ST7789 — the default_envs board (esp32dev_cyd)
#endif
#ifndef PANEL_NATIVE_H
#define PANEL_NATIVE_H 320
#endif
#ifndef PANEL_ROTATION
#define PANEL_ROTATION 1
#endif
#ifndef PANEL_PX_PER_MM_X100
#define PANEL_PX_PER_MM_X100 560
#endif

namespace panel {

// LovyanGFX setRotation() semantics: 0 = the panel's native orientation, 1/2/3 = +90/180/270.
// Both CYD panels are native PORTRAIT, so an odd rotation is landscape on either board.
constexpr int kRotation = PANEL_ROTATION;
static_assert(kRotation >= 0 && kRotation <= 3, "PANEL_ROTATION must be 0..3");

constexpr int32_t kNativeW = PANEL_NATIVE_W;
constexpr int32_t kNativeH = PANEL_NATIVE_H;
constexpr bool kSwapped = (kRotation & 1) != 0;

// Screen geometry AFTER rotation — what LVGL, the flush and every layout see.
constexpr int32_t W = kSwapped ? kNativeH : kNativeW;
constexpr int32_t H = kSwapped ? kNativeW : kNativeH;
constexpr bool kPortrait = H > W;

// Pixel pitch. Square pixels on both panels, so one scalar suffices. Scaled x100 because a -D
// cannot carry a float. 2.8" 320x240 = 5.60; 3.5" 320x480 = 6.49 (see the [board_*] sections).
constexpr int32_t kPxPerMmX100 = PANEL_PX_PER_MM_X100;

// Millimetres (x10) -> pixels, half-up. Integer-only on purpose: these numbers are a layout
// contract that theme.h static_asserts, and constexpr float would make it depend on the host's
// rounding.
constexpr int32_t pxFromMmX10(int32_t mm_x10) {
  return (mm_x10 * kPxPerMmX100 + 500) / 1000;
}

} // namespace panel
