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

// Press-feedback timing. `static` because LVGL stores the POINTER, not a copy — and plain PODs,
// not lv_style_t, so unlike a shared style they survive the native tests' lv_init()/lv_deinit()
// cycles (see the file header).
static const lv_style_prop_t kFeedbackProps[] = {LV_STYLE_BG_OPA, LV_STYLE_BG_COLOR,
                                                 LV_STYLE_BORDER_OPA, LV_STYLE_BORDER_COLOR,
                                                 LV_STYLE_PROP_INV};
static lv_style_transition_dsc_t g_press_tr;
static lv_style_transition_dsc_t g_release_tr;

static void ensure_feedback_transitions() {
  static bool ready = false;
  if (ready) {
    return;
  }
  // Entering pressed: INSTANT. The design guide's budget is a visible reaction within 100 ms, and
  // the surest way never to miss it is not to animate the acknowledgement at all — a press is a
  // fact, not a transition. lv_theme_default would otherwise fade it in over 80 ms.
  lv_style_transition_dsc_init(&g_press_tr, kFeedbackProps, lv_anim_path_linear, 0, 0, nullptr);
  // Releasing: a short ease-out so the control doesn't snap. Free — the finger has already gone,
  // and nothing is waiting on it.
  lv_style_transition_dsc_init(&g_release_tr, kFeedbackProps, lv_anim_path_ease_out, 120, 0,
                               nullptr);
  ready = true;
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

  // A framed region over the canvas rather than a slab on it: part-transparent fill so the dot
  // grid reads through, plus an accent outline and brackets that say "control" where the fill no
  // longer does. The pressed state below is a LARGER delta than a slab's shade change.
  lv_obj_set_style_bg_opa(btn, LV_OPA_50, 0);
  add_hairline(btn);
  add_brackets(btn); // every button is clickable, so every button is bracketed — see the rule
  // Pressed goes fully opaque AND brightens its edge — unmissable through a nitrile glove.
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
  lv_obj_set_style_border_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
  // Disabled recedes completely: no edge, and brackets_draw_cb withholds the corners. A control
  // you cannot use must stop looking like a control (ISA-101) — the opposite of LVGL's default,
  // which BRIGHTENS it.
  lv_obj_set_style_bg_opa(btn, LV_OPA_30, LV_STATE_DISABLED);
  lv_obj_set_style_border_opa(btn, LV_OPA_TRANSP, LV_STATE_DISABLED);

  // Cancel lv_theme_default's press "grow" (LV_THEME_DEFAULT_GROW, lv_conf.h): it sets
  // transform_width/height on pressed, and a transform forces LVGL to snapshot the widget to an
  // intermediate layer before scaling it. A 320x170 mode tile is ~54,000 pixels, i.e. >100 kB at
  // RGB565, snapshotted per frame on a board with no PSRAM and partial draw buffers — so the press
  // did not merely animate for its nominal 80 ms, it took visibly longer than that to render.
  // Killing the transform makes the acknowledgement a pure fill/edge change: a plain blit.
  //
  // Neutralised per-button rather than by flipping LV_THEME_DEFAULT_GROW, for the same reason as
  // recolor_opa above: that switch restyles every widget in LVGL to fix one state of ours.
  lv_obj_set_style_transform_width(btn, 0, LV_STATE_PRESSED);
  lv_obj_set_style_transform_height(btn, 0, LV_STATE_PRESSED);

  // Own the feedback timing rather than inheriting lv_theme_default's 80 ms fade: instant into
  // pressed, a short ease back out.
  ensure_feedback_transitions();
  lv_obj_set_style_transition(btn, &g_press_tr, LV_STATE_PRESSED);
  lv_obj_set_style_transition(btn, &g_release_tr, LV_STATE_DEFAULT);

  // Switch off LVGL's own state tinting, which otherwise overrules both lines above.
  //
  // lv_theme_default gives its pressed/disabled styles a `recolor` (lv_theme_default.c: 35/255
  // toward grey when pressed, LV_OPA_50 when disabled). recolor is a POST-PROCESS: it blends the
  // finished pixels, so it does not lose to a local bg_color the way an ordinary style property
  // would — setting TILE_DISABLED does not evict it, it just gets blended.
  //
  // And lv_conf.h has LV_THEME_DEFAULT_DARK = 0, so LVGL thinks it is theming a LIGHT UI and its
  // disabled recolor is lv_palette_lighten(GREY, 2) ≈ #EEEEEE. A disabled tile therefore rendered
  // #7b7d84 — LIGHTER and more eye-catching than the TILE-filled enabled one, which is precisely
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
  // No brackets, and no hairline. A row consumes a tap, but it is not a touch target: it is
  // SELECTED, and the thing you press to act on it is the Open button in the footer (§23/§24).
  // Brackets mean "press me" — spending them on a row spends the signal on the wrong widget and
  // makes a list of them read as noise.
}

// ---------------------------------------------------------------------------------------------
// Line-art — the FUI structure.
// ---------------------------------------------------------------------------------------------

void add_hairline(lv_obj_t *obj) {
  // All four sides. A rule along the top edge alone reads as an underline on whatever sits above
  // it rather than as an outline of this thing, and it leaves the other three edges defined only
  // by a fill — which on glass whose blacks sit grey is barely an edge at all.
  lv_obj_set_style_border_width(obj, HAIRLINE, 0);
  lv_obj_set_style_border_side(obj, LV_BORDER_SIDE_FULL, 0);
  lv_obj_set_style_border_color(obj, col(ACCENT), 0);
  // Under full opacity — structure must not compete with the datum sitting on it; at LV_OPA_COVER
  // an accent line reads as loud as an alarm. But not far under: at 40% it vanished entirely.
  lv_obj_set_style_border_opa(obj, LV_OPA_50, 0);
}

// Corner brackets, drawn ONLY while the object can actually be pressed.
//
// The affordance rule, made literal: a bracket means "you can press this". It reads
// LV_OBJ_FLAG_CLICKABLE — the SAME flag the click routing reads — so the mark and the behaviour
// cannot drift apart. Home binds that flag to the controller link (§9/§14), so when the link drops
// the mode tiles lose their brackets in the same frame, and nothing here knows what a link is.
//
// Drawn rather than assembled from child objects: a draw callback sees the object's live state for
// free (a child does not inherit its parent's state, so child brackets could not do this), costs no
// widgets, and lands on the outer bounds without having to undo the caller's padding.
static void brackets_draw_cb(lv_event_t *e) {
  lv_obj_t *obj = lv_event_get_target_obj(e);
  if (!lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE) || lv_obj_has_state(obj, LV_STATE_DISABLED)) {
    return;
  }

  lv_layer_t *layer = lv_event_get_layer(e);
  lv_area_t c;
  lv_obj_get_coords(obj, &c);

  lv_draw_rect_dsc_t dsc;
  lv_draw_rect_dsc_init(&dsc);
  dsc.bg_color = col(ACCENT);
  dsc.bg_opa = LV_OPA_COVER;
  dsc.radius = 0;

  const int32_t t = BRACKET_W - 1; // lv_area_t is inclusive on both edges
  const int32_t l = BRACKET - 1;

  // Two arms per corner, on the object's OUTER bounds — so the hairline's 1 px ring falls INSIDE
  // the bracket's 2 px and the corner reads as a thickening of the outline, not as a second mark
  // offset from it.
  const lv_area_t arms[] = {
      {c.x1, c.y1, c.x1 + l, c.y1 + t}, {c.x1, c.y1, c.x1 + t, c.y1 + l},
      {c.x2 - l, c.y1, c.x2, c.y1 + t}, {c.x2 - t, c.y1, c.x2, c.y1 + l},
      {c.x1, c.y2 - t, c.x1 + l, c.y2}, {c.x1, c.y2 - l, c.x1 + t, c.y2},
      {c.x2 - l, c.y2 - t, c.x2, c.y2}, {c.x2 - t, c.y2 - l, c.x2, c.y2},
  };
  for (const lv_area_t &a : arms) {
    lv_draw_rect(layer, &dsc, &a);
  }
}

void add_brackets(lv_obj_t *obj) {
  lv_obj_add_event_cb(obj, brackets_draw_cb, LV_EVENT_DRAW_MAIN_END, nullptr);
}

// The Oblivion dot matrix. Drawn in an event rather than pre-rendered to an lv_canvas because a
// full-screen RGB565 canvas is ~300 kB and this board has no PSRAM (design.md §6a).
//
// MUST stay clipped to the layer's clip area, and that is not an optimisation — it is the
// difference between usable and not. With no PSRAM the display renders in PARTIAL scanline chunks
// (DRAW_BUF_LINES, cyd_board.h), so this callback fires once PER CHUNK, ~a dozen times per full
// screen. Queueing every dot each time cost ~7,200 draw tasks per screen instead of ~600, and a
// Home -> Settings transition took about 3 seconds on the real 3.5" panel. Clipped, each chunk
// queues only the dots inside it.
static void grid_draw_cb(lv_event_t *e) {
  lv_obj_t *obj = lv_event_get_target_obj(e);
  lv_layer_t *layer = lv_event_get_layer(e);

  lv_area_t coords;
  lv_obj_get_coords(obj, &coords);

  // _clip_area is the "current clip area for draw tasks being added", which is exactly what this
  // callback is doing. (Its header warns draw UNITS off it — a different phase, not this one.)
  //
  // Intersected by hand rather than with lv_area_intersect(): that lives in lv_area_private.h, and
  // a private LVGL header is not something production UI code should be pinned to for four lines
  // of min/max.
  lv_area_t clip;
  clip.x1 = LV_MAX(coords.x1, layer->_clip_area.x1);
  clip.y1 = LV_MAX(coords.y1, layer->_clip_area.y1);
  clip.x2 = LV_MIN(coords.x2, layer->_clip_area.x2);
  clip.y2 = LV_MIN(coords.y2, layer->_clip_area.y2);
  if (clip.x1 > clip.x2 || clip.y1 > clip.y2) {
    return; // this chunk does not touch the screen's area at all
  }

  lv_draw_rect_dsc_t dsc;
  lv_draw_rect_dsc_init(&dsc);
  dsc.bg_color = col(GRID);
  dsc.bg_opa = LV_OPA_COVER;
  dsc.radius = 0;

  // Start on the lattice, not on the clip edge, so the dots land in the same absolute places no
  // matter which chunk happens to be redrawing — otherwise the grid would visibly shift with the
  // dirty region.
  const int32_t x0 = coords.x1 + ((clip.x1 - coords.x1) / GRID_STEP) * GRID_STEP;
  const int32_t y0 = coords.y1 + ((clip.y1 - coords.y1) / GRID_STEP) * GRID_STEP;

  for (int32_t y = y0; y <= clip.y2; y += GRID_STEP) {
    for (int32_t x = x0; x <= clip.x2; x += GRID_STEP) {
      lv_area_t dot = {x, y, x, y}; // a single pixel: anything larger reads as texture, not grid
      lv_draw_rect(layer, &dsc, &dot);
    }
  }
}

void add_dot_grid(lv_obj_t *scr) {
  // MAIN_END, not MAIN: the dots sit on top of the screen's background fill but under every
  // panel, so panels occlude them exactly as opaque surfaces should.
  lv_obj_add_event_cb(scr, grid_draw_cb, LV_EVENT_DRAW_MAIN_END, nullptr);
}

void apply_alert(lv_obj_t *obj, uint32_t hue) {
  // A wash rather than a slab. A fully-saturated amber bar would out-shout the red one beside it,
  // and severity has to be ordered by saturation, not by area (design guide / ISA-101). The fill
  // only has to say "this bar is amber" — the edge and the text carry the hue at full strength,
  // so the fill can sit this far back and the banner still reads instantly.
  lv_obj_set_style_bg_color(obj, col(hue), 0);
  lv_obj_set_style_bg_opa(obj, LV_OPA_10, 0);
  lv_obj_set_style_border_width(obj, HAIRLINE, 0);
  lv_obj_set_style_border_color(obj, col(hue), 0);
  lv_obj_set_style_border_opa(obj, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(obj, RADIUS, 0);
  lv_obj_set_style_text_color(obj, col(hue), 0);
  lv_obj_set_style_pad_hor(obj, PAD_M, 0);
  lv_obj_set_style_pad_ver(obj, PAD_S, 0);
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

void apply_pill(lv_obj_t *obj, uint32_t hue) {
  lv_obj_set_style_bg_color(obj, col(hue), 0);
  lv_obj_set_style_bg_opa(obj, LV_OPA_10, 0); // same reasoning as apply_alert: edge + text carry it
  lv_obj_set_style_border_width(obj, HAIRLINE, 0);
  lv_obj_set_style_border_color(obj, col(hue), 0);
  lv_obj_set_style_border_opa(obj, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_text_color(obj, col(hue), 0);
  lv_obj_set_style_pad_hor(obj, PAD_S, 0);
  lv_obj_set_style_pad_ver(obj, 0, 0);
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

void apply_fault_panel(lv_obj_t *obj) {
  // Opaque, not tinted: the fault overlay is modal, and anything showing through it would suggest
  // the screen underneath is still live. A 2 px edge — the only place the theme goes above a
  // hairline, because this is the one panel that must not read as structure.
  lv_obj_set_style_bg_color(obj, col(SURFACE), 0);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(obj, 2, 0);
  lv_obj_set_style_border_color(obj, col(FAULT), 0);
  lv_obj_set_style_border_opa(obj, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(obj, RADIUS, 0);
  lv_obj_set_style_text_color(obj, col(TEXT), 0);
  lv_obj_set_style_pad_all(obj, PAD_M, 0);
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static void anim_bg_opa_cb(void *var, int32_t v) {
  lv_obj_set_style_bg_opa(static_cast<lv_obj_t *>(var), static_cast<lv_opa_t>(v), 0);
}

void alarm_pulse(lv_obj_t *obj) {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, obj);
  lv_anim_set_exec_cb(&a, anim_bg_opa_cb);
  // Pulses the FILL only, between the resting wash and a little brighter. Not the border and not
  // the text: a blinking word is harder to read, and an alarm you cannot read is decoration.
  // The floor matches apply_alert's resting fill, so the pulse reads as the banner breathing
  // rather than as a second, brighter banner appearing on top of it.
  lv_anim_set_values(&a, LV_OPA_10, LV_OPA_40);
  lv_anim_set_duration(&a, 600);
  lv_anim_set_playback_duration(&a, 600);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_start(&a);
}

void apply_caption(lv_obj_t *label) {
  lv_obj_set_style_text_color(label, col(TEXT_DIM), 0);
  // Tracking is what separates a caption from just-smaller-text.
  lv_obj_set_style_text_letter_space(label, 1, 0);
}

} // namespace theme
