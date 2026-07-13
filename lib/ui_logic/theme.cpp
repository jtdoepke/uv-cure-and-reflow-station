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
}

void apply_mode_tile(lv_obj_t *btn) {
  apply_button_base(btn);
}

void apply_secondary(lv_obj_t *btn) {
  apply_button_base(btn);
}

} // namespace theme
