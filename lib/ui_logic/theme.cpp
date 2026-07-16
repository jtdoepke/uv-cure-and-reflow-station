#include "theme.h"

namespace theme {

void apply_screen(lv_obj_t *scr) {
  lv_obj_set_style_bg_color(scr, col(BG), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(scr, col(TEXT), 0);
  lv_obj_set_style_border_width(scr, 0, 0);
  lv_obj_set_style_radius(scr, 0, 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
}

void apply_panel(lv_obj_t *obj) {
  lv_obj_set_style_bg_color(obj, col(SURFACE), 0);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(obj, RADIUS, 0);
  lv_obj_set_style_border_width(obj, 0, 0);
  // Propagate the light text colour so child labels don't inherit the default theme's dark
  // text (which would vanish on our dark base).
  lv_obj_set_style_text_color(obj, col(TEXT), 0);
  lv_obj_set_style_pad_hor(obj, PAD_M, 0);
  lv_obj_set_style_pad_ver(obj, PAD_S, 0);
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

void apply_row(lv_obj_t *obj) {
  lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(obj, 0, 0);
  lv_obj_set_style_text_color(obj, col(TEXT), 0);
  lv_obj_set_style_pad_all(obj, 0, 0);
  lv_obj_set_style_pad_gap(obj, GAP, 0);
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

// Shared look for the tappable buttons; the disabled treatment is what the mode tiles show
// when the link is unhealthy (§14 — no run flow without a healthy controller link).
static void apply_button_base(lv_obj_t *btn) {
  lv_obj_set_style_bg_color(btn, col(TILE), 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(btn, RADIUS, 0);
  lv_obj_set_style_border_width(btn, 0, 0);
  lv_obj_set_style_text_color(btn, col(TEXT), 0);
  lv_obj_set_style_shadow_width(btn, 0, 0);
  lv_obj_set_style_bg_color(btn, col(TILE_PRESSED), LV_STATE_PRESSED);
  lv_obj_set_style_bg_color(btn, col(TILE_DISABLED), LV_STATE_DISABLED);
  lv_obj_set_style_text_color(btn, col(TEXT_DIM), LV_STATE_DISABLED);

  // Switch off LVGL's own state tinting, which otherwise overrules both lines above.
  //
  // lv_theme_default gives its pressed/disabled styles a `recolor` (lv_theme_default.c: 35/255
  // toward grey when pressed, LV_OPA_50 when disabled). recolor is a POST-PROCESS: it blends the
  // finished pixels, so it does not lose to a local bg_color the way an ordinary style property
  // would — setting TILE_DISABLED does not evict it, it just gets blended.
  //
  // And lv_conf.h has LV_THEME_DEFAULT_DARK = 0, so LVGL thinks it is theming a LIGHT UI and its
  // disabled recolor is lv_palette_lighten(GREY, 2) ≈ #EEEEEE. A disabled tile therefore rendered
  // #7b7d84 — LIGHTER and more eye-catching than the #2a2f3a enabled one, which is precisely
  // backwards for ISA-101, where a control you cannot use must recede. On the Home screen with no
  // controller attached, that is two big pale slabs filling the display: the whole screen reads
  // washed out, and grepping theme.h for the colour finds nothing, because it is not in there.
  //
  // Neutralising recolor_opa (rather than flipping LV_THEME_DEFAULT_DARK, which would restyle
  // every widget in LVGL to fix two states of ours) leaves TILE_PRESSED/TILE_DISABLED as the
  // exact, only answer — which is what this file claims to be.
  lv_obj_set_style_recolor_opa(btn, LV_OPA_TRANSP, LV_STATE_PRESSED);
  lv_obj_set_style_recolor_opa(btn, LV_OPA_TRANSP, LV_STATE_DISABLED);
}

void apply_mode_tile(lv_obj_t *btn) {
  apply_button_base(btn);
}

void apply_secondary(lv_obj_t *btn) {
  apply_button_base(btn);
}

void apply_stepper_button(lv_obj_t *btn) {
  apply_button_base(btn); // press + disabled (at min/max) treatment come for free
  lv_obj_set_size(btn, STEPPER_BTN, STEPPER_BTN);
  lv_obj_set_style_text_font(btn, &big_font(), 0); // big −/+ glyph
}

void apply_keypad_key(lv_obj_t *btn) {
  apply_button_base(btn); // press + disabled (OK out-of-range) treatment come for free
  // Size is left to the caller's flex grid (keys ≈ 74×60 px, §26); only the big glyph is fixed —
  // the same 28 px font that carries the digits also carries the ⌫/✓/✕ icons.
  lv_obj_set_style_text_font(btn, &big_font(), 0);
}

void apply_list_row(lv_obj_t *obj) {
  lv_obj_set_style_bg_color(obj, col(SURFACE), 0);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(obj, RADIUS, 0);
  lv_obj_set_style_border_width(obj, 0, 0);
  lv_obj_set_style_text_color(obj, col(TEXT), 0);
  lv_obj_set_style_shadow_width(obj, 0, 0);
  lv_obj_set_style_pad_hor(obj, PAD_M, 0);
  lv_obj_set_style_pad_ver(obj, 0, 0);
  // The selected treatment (bg SELECTED) is toggled by the list's observer, not baked here.
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

} // namespace theme
