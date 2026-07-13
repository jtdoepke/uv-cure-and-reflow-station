// theme — the single place the UI's visual language is defined. Follows the ISA-101 /
// IEC 63303 high-performance-HMI discipline (see the ui-development design guide): a neutral
// grayscale base with colour reserved for machine state (green idle · amber hot · red fault),
// never decorative. Screens read colours/geometry from the constexpr tokens and style their
// widgets through the apply_* helpers, so every screen looks the same and one edit here
// restyles the whole UI.
//
// LVGL-only (no <Arduino.h>, no LovyanGFX): compiles for the firmware and the native_ui_cyd /
// native_sim host targets alike.
//
// Note on styling strategy: the helpers apply properties inline (stored in each widget's own
// style list, freed with the widget) rather than through process-lifetime shared lv_style_t
// globals. That keeps a single source of truth while staying safe across the native tests'
// repeated lv_init()/lv_deinit() cycles, which reset LVGL's allocator out from under any
// long-lived style. Introduce a shared lv_style_t only for a screen with many repeated
// widgets (e.g. a long list), where the per-widget copies would actually cost RAM.
#pragma once

#include <lvgl.h>

// Larger Red Hat Mono for big numeric readouts (value-stepper §24). Applied per-widget via
// lv_obj_set_style_text_font; the global LV_FONT_DEFAULT stays 14 px. Generated + committed
// under fonts/ (see fonts/README.md). Declared here so any view may use it.
LV_FONT_DECLARE(red_hat_mono_28)

namespace theme {

// Palette — neutral grayscale base + three reserved state colours (never used decoratively).
constexpr uint32_t BG = 0x101216;      // screen background
constexpr uint32_t SURFACE = 0x1b1f27; // header / status-band surface
constexpr uint32_t TILE = 0x2a2f3a;    // neutral mode / secondary buttons
constexpr uint32_t TILE_PRESSED = 0x3a414f;
constexpr uint32_t TILE_DISABLED = 0x191c22;
constexpr uint32_t SELECTED = 0x35507a; // highlighted list row (§23/§24 ▲/▼ selection) — a
                                        // desaturated blue, distinct from the reserved state hues
constexpr uint32_t TEXT = 0xeceff4;     // primary text
constexpr uint32_t TEXT_DIM = 0x8a93a3; // secondary / disabled text
constexpr uint32_t IDLE = 0x3fae6b;     // green — safe / normal
constexpr uint32_t WARN = 0xe0a52b;     // amber — hot / warning
constexpr uint32_t FAULT = 0xd94f3d;    // red   — danger (reserved)

// Geometry. The panel is ~5.6 px/mm, so 56 px ≈ 10 mm is the touch-target floor; the mode
// tiles are far larger, the secondary row clears the floor.
constexpr int32_t PAD_S = 6, PAD_M = 10, GAP = 8, RADIUS = 6;
constexpr int32_t HEADER_H = 30, BAND_H = 42, BANNER_H = 24;
constexpr int32_t TILE_H = 104, SECONDARY_H = 58, TOUCH_MIN = 56;
constexpr int32_t DOT = 14;         // state indicator dot
constexpr int32_t STEPPER_BTN = 96; // value-stepper −/+ (§24: ~17 mm, 15–20 mm gloved band)
// List rows (§23/§24) are navigated by the big ▲/▼ footer, so they need not be touch targets and
// can be compact.
constexpr int32_t LIST_ROW_H = 40;

inline lv_color_t col(uint32_t hex) {
  return lv_color_hex(hex);
}

// Per-widget styling helpers (see the file header for why these are inline, not shared styles).
void apply_screen(lv_obj_t *scr);         // root: background, text colour, no scroll/border
void apply_panel(lv_obj_t *obj);          // header / status-band surface
void apply_row(lv_obj_t *obj);            // transparent flex-row layout container
void apply_mode_tile(lv_obj_t *btn);      // big primary-but-non-hazardous mode button
void apply_secondary(lv_obj_t *btn);      // secondary-row button
void apply_stepper_button(lv_obj_t *btn); // large −/+ button, big glyph (value-stepper, §24)
void apply_keypad_key(lv_obj_t *btn);     // keypad digit/control key, big glyph (keypad, §26)
void apply_list_row(lv_obj_t *obj);       // selectable-list row surface (§23/§24)

} // namespace theme
