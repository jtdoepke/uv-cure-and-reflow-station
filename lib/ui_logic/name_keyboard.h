// name_keyboard.h — the shared compact on-screen keyboard for short profile / phase names
// (§12/§26). Used by the profile editor (name a new / save-as profile, rename a phase) and the
// profile library (rename a profile), so the keymap and the build recipe live in one place.
//
// LVGL's default keyboard uses control glyphs the body font deliberately doesn't carry — the
// newline ↵ (0xF8A2) isn't even in Font Awesome free — and its 12-column layout makes each key far
// too narrow on a 320 px panel. This is a compact map for short names: letters, ⌫ backspace
// (0xF55A, added to the body font to match the numeric keypad) and ✓ accept (LV_SYMBOL_OK →
// LV_EVENT_READY) — every glyph present in the font. Mode switches use the literal "abc"/"ABC"/"1#"
// strings the keyboard's own handler matches. There is no cancel key: the header Back button is the
// single cancel path. Dropping the cursor/newline/hide/cancel keys widens every remaining key.
//
// The maps + ctrl arrays are `inline` (one definition across TUs): LVGL keeps the pointers, so they
// must outlive the keyboard.
//
// LVGL-only; compiles for firmware and the native_ui_cyd / native_sim host targets.
#pragma once

#include <lvgl.h>

#include "panel.h"

namespace name_kb {

constexpr unsigned KB_CTL = LV_KEYBOARD_CTRL_BUTTON_FLAGS; // a mode-switch / OK key
constexpr unsigned KB_BSP =
    LV_BUTTONMATRIX_CTRL_CHECKED; // backspace (styled, wide; repeats on hold)
constexpr unsigned KB_POP = LV_BUTTONMATRIX_CTRL_POPOVER;   // enlarge the key in a popover on press
constexpr unsigned KB_NRP = LV_BUTTONMATRIX_CTRL_NO_REPEAT; // one char per touch (no auto-repeat)

// A ctrl-map entry: relative width + optional flags, cast to the strongly-typed LVGL enum (LVGL's
// own C maps rely on the implicit int→enum conversion this C++ TU doesn't get).
constexpr lv_buttonmatrix_ctrl_t kbw(unsigned width, unsigned flags = 0) {
  return static_cast<lv_buttonmatrix_ctrl_t>(width | flags);
}

// A character key: width 1, a phone-style popover preview above it while pressed (the narrow keys
// are easy to fat-finger, so the popover shows which letter is under the touch), and no auto-repeat
// so holding to read the preview types exactly one character rather than a run of them.
constexpr lv_buttonmatrix_ctrl_t P = kbw(1, KB_POP | KB_NRP);

// Hand-aligned so each map row mirrors the on-screen row (and its ctrl entry lines up beneath it).
// clang-format off
inline const char *const kLower[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
    "ABC", "z", "x", "c", "v", "b", "n", "m", LV_SYMBOL_BACKSPACE, "\n",
    "1#", " ", LV_SYMBOL_OK, ""};
inline const char *const kUpper[] = {
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", "\n",
    "abc", "Z", "X", "C", "V", "B", "N", "M", LV_SYMBOL_BACKSPACE, "\n",
    "1#", " ", LV_SYMBOL_OK, ""};
// Shared 32-entry ctrl map for the two letter layouts (identical key geometry).
inline const lv_buttonmatrix_ctrl_t kCtrl[] = {
    P, P, P, P, P, P, P, P, P, P,                                                           // q..p
    P, P, P, P, P, P, P, P, P,                                                               // a..l
    kbw(2, KB_CTL), P, P, P, P, P, P, P, kbw(2, KB_BSP),                                     // ABC z..m ⌫
    kbw(2, KB_CTL), kbw(6), kbw(2, KB_CTL)};                                                 // 1# space ✓

inline const char *const kSpecial[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "-", "_", "/", ":", ";", "(", ")", "&", "@", "\n",
    "abc", ".", ",", "?", "!", "'", "+", "#", LV_SYMBOL_BACKSPACE, "\n",
    " ", LV_SYMBOL_OK, ""};
inline const lv_buttonmatrix_ctrl_t kSpecialCtrl[] = {
    P, P, P, P, P, P, P, P, P, P,                                                           // 1..0
    P, P, P, P, P, P, P, P, P,                                                               // - _ / : ; ( ) & @
    kbw(2, KB_CTL), P, P, P, P, P, P, P, kbw(2, KB_BSP),                                     // abc . , ? ! ' + # ⌫
    kbw(8), kbw(2, KB_CTL)};                                                                 // space ✓
// clang-format on

// Build the name-entry keyboard under `parent`, bound to `textarea`, firing `on_ready` (with
// `user_data`) when the ✓ key is pressed (LV_EVENT_READY). Sizes the keys ~square within the short
// landscape panel. Returns the keyboard object.
inline lv_obj_t *create(lv_obj_t *parent, lv_obj_t *textarea, lv_event_cb_t on_ready,
                        void *user_data) {
  lv_obj_t *kb = lv_keyboard_create(parent);
  lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_LOWER, kLower, kCtrl);
  lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_UPPER, kUpper, kCtrl);
  lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_SPECIAL, kSpecial, kSpecialCtrl);
  lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_keyboard_set_popovers(kb, true); // honor the POPOVER ctrl flags — enlarge each pressed key
  lv_keyboard_set_textarea(kb, textarea);
  // Bound the height so 4 rows read ~square, not tall-and-narrow (mm-authored, capped so it still
  // fits the short landscape panel). flex_grow would stretch it over the whole lower screen.
  int32_t kb_h = panel::pxFromMmX10(360); // ~36 mm of keys
  const int32_t kb_cap = (panel::H * 55) / 100;
  if (kb_h > kb_cap) {
    kb_h = kb_cap;
  }
  lv_obj_set_width(kb, lv_pct(100));
  lv_obj_set_height(kb, kb_h);
  lv_obj_add_event_cb(kb, on_ready, LV_EVENT_READY, user_data);
  return kb;
}

} // namespace name_kb
