// theme — the single place the UI's visual language is defined ("Azure Instrument", design.md
// §14). Follows the ISA-101 / IEC 63303 high-performance-HMI discipline (see the ui-development
// design guide): a neutral near-black base, ~90% of the screen neutral, colour reserved for
// machine state (green idle · amber hot · red fault) and never decorative, plus ONE non-state
// accent carrying structure and live data. Screens read colours/geometry from the constexpr
// tokens and style their widgets through the apply_*/add_* helpers, so every screen looks the
// same and one edit here restyles the whole UI.
//
// The sci-fi look and the ISA-101 rules are the same visual grammar, which is why this is a
// palette plus some line-art rather than a second, competing design.
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

// Larger Red Hat Mono for big numeric readouts (value-stepper §24) and glove-sized keys (§26).
// Applied per-widget via lv_obj_set_style_text_font. Generated + committed under fonts/ (see
// fonts/README.md). Declared here so any view may use it.
//
// Sized by the panel's PITCH, not by a board name — glyphs are the one thing the mm-authored
// tokens below cannot scale, because a font is a fixed grid of pixels. A 28 px glyph is 5.0 mm at
// 5.60 px/mm but only 4.3 mm at 6.49; 32 px puts it back to 4.9 mm. Same argument for the 14/16 px
// LV_FONT_DEFAULT, which platformio.ini sets per board.
//
// A pitch threshold rather than `#if CYD_BOARD_...` for the usual reason: lib/ must never learn a
// board identity, and pitch is the property this actually depends on — a third board at 6.5 px/mm
// gets the right answer with no edit here. The unused size is not declared, so it never links.
#if PANEL_PX_PER_MM_X100 >= 600
#define THEME_BIG_FONT red_hat_mono_32
#else
#define THEME_BIG_FONT red_hat_mono_28
#endif
LV_FONT_DECLARE(THEME_BIG_FONT)

namespace theme {

// The big-readout font, as a reference the views use without naming a size.
inline const lv_font_t &big_font() {
  return THEME_BIG_FONT;
}

// Palette — "Azure Instrument" (chosen 2026-07-15 from a rendered variant sweep; design.md §14).
//
// A near-black canvas with ~90% of the screen neutral, one non-state accent carrying structure and
// live data, and three reserved state hues that appear for nothing else. This is the ISA-101
// discipline and the cinematic-FUI look at the same time — they are the same visual grammar, which
// is why the restyle cost a palette and some line-art rather than a redesign.
constexpr uint32_t BG = 0x05070a;      // near-black canvas
constexpr uint32_t SURFACE = 0x0c1116; // header / status-band surface
constexpr uint32_t TILE = 0x161d26; // mode / secondary buttons (drawn as outlines, see theme.cpp)
constexpr uint32_t TILE_PRESSED = 0x24303d;
constexpr uint32_t TILE_DISABLED = 0x0a0e12;
constexpr uint32_t SELECTED = 0x0e3357; // highlighted list row (§23/§24 ▲/▼) — accent-tinted, and
                                        // distinct from every reserved state hue
constexpr uint32_t TEXT = 0xe3eaf0;     // primary text — 16.6:1 on BG, clears the 7:1 readout floor
constexpr uint32_t TEXT_DIM = 0x727e89; // secondary / disabled text / captions
constexpr uint32_t IDLE = 0x35d07f;     // green — safe / normal
constexpr uint32_t WARN = 0xffb020;     // amber — hot / warning
constexpr uint32_t FAULT = 0xff3b30;    // red   — danger (reserved)

// Contrast, measured rather than asserted (WCAG ratio, against BG / SURFACE):
//   TEXT 16.61 / 15.62 · WARN 11.03 / 10.37 · IDLE 10.07 / 9.46
//   FAULT 5.69 / 5.35 · ACCENT 5.53 / 5.20 · TEXT_DIM 4.86 / 4.57
// TEXT_DIM is set exactly where it clears the 4.5 body floor on SURFACE, the worse of its two
// grounds — at its previous #6e7a85 a caption on a panel measured 4.32 and quietly failed.
//
// The saturated hues sit at ~5.2-5.7: above the 4.5 floor, below the 7:1 one the design guide
// wants for critical readouts. That is a property of saturated red/blue on near-black, not an
// oversight — dragging FAULT to 7:1 turns it #ff6b60, i.e. pink, trading the meaning of "red"
// for a number. The readouts themselves are TEXT and do clear 7:1; the hues only ever carry
// state, and state is ALWAYS redundantly coded with a word and a glyph. That redundancy is what
// makes this safe, which is why it is a rule and not a preference.

// The accent: structure, and "this is the live datum". A fourth, NON-state hue — it must never be
// readable as green/amber/red, or it stops meaning "look here" and starts meaning "something is
// wrong". Azure is a true blue rather than a cyan precisely because it is the furthest of the
// candidates from all three state hues; a teal accent sat close enough to IDLE green to be
// confusable at a glance through a glove, which is the failure this rule exists to prevent.
constexpr uint32_t ACCENT = 0x0a84ff;
constexpr uint32_t ACCENT_DIM = 0x14456f; // the accent, receded — for rules that must not shout
constexpr uint32_t GRID = 0x24384e;       // background dot matrix; barely-there by construction

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

// Line-art geometry (the FUI structure). Not covered by the static_asserts below: these are new
// tokens, so there is no 2.8" original for them to drift from.
constexpr int32_t HAIRLINE = 1;  // px: a rule is a rendering detail, like RADIUS
constexpr int32_t BRACKET_W = 2; // px, likewise — and deliberately 2x HAIRLINE, so a bracket reads
                                 // as a thickening of the outline it sits on rather than a
                                 // separate mark beside it
constexpr int32_t BRACKET = panel::pxFromMmX10(20);   // corner-bracket arm length
constexpr int32_t GRID_STEP = panel::pxFromMmX10(25); // dot-matrix pitch

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

// Line-art — the structure that makes the panel read as instrumentation rather than as an app.
//
// All of it is drawn with widget BORDERS, not glyphs: the fonts carry ASCII + ° + the handful of
// Font Awesome icons listed in fonts/README.md, and no box-drawing characters. Borders also scale
// with the mm tokens, where a glyph would be frozen at one pixel size.
void add_hairline(lv_obj_t *obj); // 1 px accent outline, all four sides
void add_dot_grid(lv_obj_t *scr); // background dot matrix, drawn behind every panel

// Corner brackets — the "this is a touch target" mark, and the ONLY thing that carries it.
// Self-policing: it draws only while the object has LV_OBJ_FLAG_CLICKABLE and is not disabled,
// which is the same flag the click routing reads, so a bracket cannot outlive the press it
// promises. Do not spend it on things that merely consume a tap (a selectable list row is
// selected; the control you press is the footer's Open) — see design.md §14.
void add_brackets(lv_obj_t *obj);

// The dim, tracked caption of the FUI "labelled numeric column" — the label BESIDE or ABOVE a
// value. (Its partner, a big-mono readout role, waits for Run/Monitor (§15), the first screen
// with a datum worth that much of the panel.)
void apply_caption(lv_obj_t *label);

// --- Alert vocabulary (§13 cross-cutting overlays, §22 fault overlay) --------------------------
//
// `hue` is a reserved STATE colour — WARN for caution, FAULT for alarm — never ACCENT. These
// helpers style the container only: the redundant cue (a ⚠ glyph and a word like "CAUTION") is
// the caller's job, because ISA-101 forbids colour-only state and no style can enforce that.
//
// Glyph note: big_font() carries ASCII + ° + ✓ ✗ ⌫ — so ordinary words are fine in it, but ⚠, ‹
// and ▲/▼ are merged into the 14/16 px fonts ONLY. A LV_SYMBOL_WARNING set in big_font() renders
// as a missing-glyph box (fonts/README.md has the exact ranges and the regeneration recipe).

// A full-width banner: hue-tinted fill, hue edge, hue text. Caution and alarm differ only in hue.
void apply_alert(lv_obj_t *obj, uint32_t hue);

// A status pill — rounded, hue-tinted, hue-edged. Pair with a glyph + word ("✓ READY").
void apply_pill(lv_obj_t *obj, uint32_t hue);

// The §22 modal danger panel: a heavy FAULT edge over an opaque ground, so it reads as a thing
// that has taken over the screen rather than a thing drawn on it.
void apply_fault_panel(lv_obj_t *obj);

// Infinite fill-opacity pulse for an ACTIVE alarm — the one steady-state motion ISA-101 permits,
// and only ever redundant with the icon + word + colour already on the banner. Call
// lv_anim_delete(obj, nullptr) when the condition clears.
void alarm_pulse(lv_obj_t *obj);

} // namespace theme
