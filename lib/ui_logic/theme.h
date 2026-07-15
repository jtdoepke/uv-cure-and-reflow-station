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

#include "panel.h"

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

// Geometry, authored in MILLIMETRES and converted at compile time from the panel's pixel pitch
// (lib/panel/panel.h, set per board in platformio.ini). The design guide's rules are physical —
// a 10 mm touch-target floor, a 15–20 mm gloved band — so px literals only ever meant those
// things on the one panel they were hand-derived for. Arguments are mm x10.
constexpr int32_t PAD_S = panel::pxFromMmX10(11);
constexpr int32_t PAD_M = panel::pxFromMmX10(18);
constexpr int32_t GAP = panel::pxFromMmX10(7);
constexpr int32_t RADIUS = 2; // px, not mm: a 2 px corner is a rendering detail, not a size
constexpr int32_t HEADER_H = panel::pxFromMmX10(54);
constexpr int32_t BAND_H = panel::pxFromMmX10(75);
constexpr int32_t BANNER_H = panel::pxFromMmX10(43);
constexpr int32_t TILE_H = panel::pxFromMmX10(186);
constexpr int32_t SECONDARY_H = panel::pxFromMmX10(104);
constexpr int32_t TOUCH_MIN = panel::pxFromMmX10(100);     // design guide's absolute floor
constexpr int32_t DOT = panel::pxFromMmX10(25);            // state indicator dot
constexpr int32_t STEPPER_BTN = panel::pxFromMmX10(171);   // value-stepper −/+ (§24: 15–20 mm band)
constexpr int32_t KEYPAD_RAIL_W = panel::pxFromMmX10(171); // keypad's OK/back rail (§26)
// List rows (§23/§24) are navigated by the big ▲/▼ footer, so they need not be touch targets and
// can be compact.
constexpr int32_t LIST_ROW_H = panel::pxFromMmX10(71);

// The mm figures above were reverse-engineered from the 2.8" board's hand-derived px literals, so
// on THAT panel they must still produce exactly the old numbers — otherwise this conversion
// silently restyled a working UI. Pinned here rather than trusted to review.
#if PANEL_PX_PER_MM_X100 == 560
static_assert(PAD_S == 6 && PAD_M == 10 && GAP == 4, "2.8\" spacing drifted");
static_assert(HEADER_H == 30 && BAND_H == 42 && BANNER_H == 24, "2.8\" band heights drifted");
static_assert(TILE_H == 104 && SECONDARY_H == 58 && TOUCH_MIN == 56, "2.8\" tiles drifted");
static_assert(DOT == 14 && STEPPER_BTN == 96 && LIST_ROW_H == 40, "2.8\" widgets drifted");
static_assert(KEYPAD_RAIL_W == 96, "2.8\" keypad rail drifted");
#endif

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
