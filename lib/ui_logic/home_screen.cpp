#include "home_screen.h"

#include <initializer_list>

#include "home_viewmodel.h"
#include "link_banner.h" // shared "Controller not responding" banner (§9/§14)
#include "panel.h"       // geometry (portrait vs landscape) — never a board identity
#include "subjects.h"
#include "theme.h"

namespace {

// --- Observers: state → view. Each pairs a colour with text/glyph (never colour alone). ---

void on_link_changed(lv_observer_t *observer, lv_subject_t *subject) {
  lv_obj_t *label = lv_observer_get_target_obj(observer);
  int link = lv_subject_get_int(subject);
  lv_label_set_text(label, HomeViewModel::linkText(link));
  lv_obj_set_style_text_color(label, theme::col(HomeViewModel::linkColor(link)), 0);
}

void on_state_label_changed(lv_observer_t *observer, lv_subject_t *subject) {
  lv_obj_t *label = lv_observer_get_target_obj(observer);
  int state = lv_subject_get_int(subject);
  lv_label_set_text(label, HomeViewModel::stateText(state));
  lv_obj_set_style_text_color(label, theme::col(HomeViewModel::stateColor(state)), 0);
}

void on_state_dot_changed(lv_observer_t *observer, lv_subject_t *subject) {
  lv_obj_t *dot = lv_observer_get_target_obj(observer);
  lv_obj_set_style_bg_color(dot, theme::col(HomeViewModel::stateColor(lv_subject_get_int(subject))),
                            0);
}

// Chamber temperature, shown in the user's chosen unit (§24). Bound to both subjects so it
// updates on a new reading and when the units setting changes; the stored temp is always °C.
void on_chamber_changed(lv_observer_t *observer, lv_subject_t *) {
  lv_obj_t *label = lv_observer_get_target_obj(observer);
  int celsius = lv_subject_get_int(&subj_chamber_temp);
  bool fahrenheit = lv_subject_get_int(&subj_units) != 0;
  int shown = fahrenheit ? celsius * 9 / 5 + 32 : celsius;
  // The word "CHAMBER" is a separate dim caption widget above this one (the FUI labelled-numeric
  // column), so the value label carries the number alone.
  lv_label_set_text_fmt(label, "%d %s", shown, fahrenheit ? "°F" : "°C");
}

// Build a labelled button and route its click to the view model. `on_click` is a captureless
// lambda (decays to lv_event_cb_t) — an lv_event_cb_t can't be a member function, so the thunk
// recovers the view model from user_data and calls the intent. Per-tile lambdas keep it simple
// and avoid smuggling a pointer-to-member through the user_data slot.
lv_obj_t *make_tile(lv_obj_t *parent, const char *text, void (*apply)(lv_obj_t *),
                    lv_event_cb_t on_click) {
  lv_obj_t *btn = lv_button_create(parent);
  apply(btn);
  lv_obj_t *label = lv_label_create(btn);
  lv_label_set_text(label, text);
  lv_obj_center(label);
  lv_obj_add_event_cb(btn, on_click, LV_EVENT_CLICKED, &home_view_model());
  return btn;
}

// The shared thunk body: recover the view model and invoke one of its intent methods.
#define HOME_INTENT(method)                                                                        \
  [](lv_event_t *e) { static_cast<HomeViewModel *>(lv_event_get_user_data(e))->method(); }

} // namespace

HomeScreen create_home_screen(lv_obj_t *parent) {
  HomeScreen ui{};
  ui.root = parent;

  theme::apply_screen(parent); // includes the dot-matrix background (every screen carries it)
  lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(parent, theme::PAD_S, 0);
  lv_obj_set_style_pad_row(parent, theme::GAP, 0);

  // --- Header: title (left) + link indicator (right) ---
  lv_obj_t *header = lv_obj_create(parent);
  theme::apply_panel(header);
  theme::add_hairline(header);
  lv_obj_set_width(header, lv_pct(100));
  lv_obj_set_height(header, theme::HEADER_H);
  lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  lv_obj_t *title = lv_label_create(header);
  lv_label_set_text(title, "Oven Controller");

  ui.link_label = lv_label_create(header);
  lv_subject_add_observer_obj(&subj_link_state, on_link_changed, ui.link_label, nullptr);

  // --- Status band: state badge (dot + word) left, live chamber temp right ---
  lv_obj_t *band = lv_obj_create(parent);
  theme::apply_panel(band);
  // Outline only — a status band is not pressable, so it gets no brackets (§14).
  theme::add_hairline(band);
  lv_obj_set_width(band, lv_pct(100));
  lv_obj_set_height(band, theme::BAND_H);
  lv_obj_set_flex_flow(band, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(band, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  lv_obj_t *badge = lv_obj_create(band);
  theme::apply_row(badge);
  lv_obj_set_size(badge, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(badge, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(badge, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_gap(badge, theme::PAD_S, 0);

  ui.state_dot = lv_obj_create(badge);
  lv_obj_set_size(ui.state_dot, theme::DOT, theme::DOT);
  lv_obj_set_style_radius(ui.state_dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(ui.state_dot, 0, 0);
  lv_obj_remove_flag(ui.state_dot, LV_OBJ_FLAG_SCROLLABLE);
  lv_subject_add_observer_obj(&subj_run_state, on_state_dot_changed, ui.state_dot, nullptr);

  ui.state_label = lv_label_create(badge);
  lv_subject_add_observer_obj(&subj_run_state, on_state_label_changed, ui.state_label, nullptr);

  // The right-hand column: a dim "CHAMBER" caption over the live value — the FUI
  // labelled-numeric column (§14).
  lv_obj_t *chamber_col = lv_obj_create(band);
  theme::apply_row(chamber_col);
  lv_obj_set_size(chamber_col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(chamber_col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(chamber_col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
  lv_obj_set_style_pad_gap(chamber_col, 0, 0);

  lv_obj_t *chamber_caption = lv_label_create(chamber_col);
  lv_label_set_text(chamber_caption, "CHAMBER");
  theme::apply_caption(chamber_caption);

  ui.chamber_label = lv_label_create(chamber_col);
  lv_subject_add_observer_obj(&subj_chamber_temp, on_chamber_changed, ui.chamber_label, nullptr);
  lv_subject_add_observer_obj(&subj_units, on_chamber_changed, ui.chamber_label, nullptr);

  // --- Banner: shown only when the link is unhealthy ---
  // The one shared "Controller not responding" banner (link_banner.h), the same object every other
  // screen carries, so a dropped link reads identically everywhere (§9/§14).
  ui.banner = create_link_banner(parent);

  // --- Mode tiles: UV CURE / REFLOW. Big, neutral, and gated on a healthy link ---
  // The tiles stack in portrait and sit side by side in landscape. Gated on panel::kPortrait —
  // the GEOMETRY — and never on a board flag: this file must keep working for any panel, and
  // "which way the long axis runs" is the only thing the decision actually depends on. Splitting
  // the long axis is what keeps a tile roughly square either way; side-by-side on a 320x480 panel
  // gives two 23x45 mm slivers, which is a landscape idea surviving into a shape that punishes it.
  lv_obj_t *modes = lv_obj_create(parent);
  theme::apply_row(modes);
  lv_obj_set_width(modes, lv_pct(100));
  lv_obj_set_flex_grow(modes, 1);
  lv_obj_set_flex_flow(modes, panel::kPortrait ? LV_FLEX_FLOW_COLUMN : LV_FLEX_FLOW_ROW);

  ui.btn_cure = make_tile(modes, "UV CURE", theme::apply_mode_tile, HOME_INTENT(onCurePressed));
  ui.btn_reflow = make_tile(modes, "REFLOW", theme::apply_mode_tile, HOME_INTENT(onReflowPressed));
  for (lv_obj_t *tile : {ui.btn_cure, ui.btn_reflow}) {
    lv_obj_set_flex_grow(tile, 1);
    // Fill the cross axis, whichever axis that now is.
    if (panel::kPortrait) {
      lv_obj_set_width(tile, lv_pct(100));
    } else {
      lv_obj_set_height(tile, lv_pct(100));
    }
    // Clickable only with a healthy link; DISABLED state also greys the tile (§14 / §9 gate).
    lv_obj_bind_flag_if_eq(tile, &subj_link_state, LV_OBJ_FLAG_CLICKABLE, LINK_OK);
    lv_obj_bind_state_if_not_eq(tile, &subj_link_state, LV_STATE_DISABLED, LINK_OK);
  }

  // --- Secondary row: Profiles / Calibrate / Settings — also gated on a healthy link ---
  // Since the §2 "CYD is a UI remote" split these are no longer CYD-local: the profile library and
  // device settings live on the CONTROLLER (§7), and Calibrate is a controller run — so every one
  // of them needs the link, exactly like the run tiles. With no controller the CYD can do nothing
  // useful, so the whole hub greys out and the "Controller not responding" banner (above) says why.
  lv_obj_t *secondary = lv_obj_create(parent);
  theme::apply_row(secondary);
  lv_obj_set_width(secondary, lv_pct(100));
  lv_obj_set_height(secondary, theme::SECONDARY_H);
  lv_obj_set_flex_flow(secondary, LV_FLEX_FLOW_ROW);

  ui.btn_profiles =
      make_tile(secondary, "Profiles", theme::apply_secondary, HOME_INTENT(onProfilesPressed));
  ui.btn_calibrate =
      make_tile(secondary, "Calibrate", theme::apply_secondary, HOME_INTENT(onCalibratePressed));
  ui.btn_settings =
      make_tile(secondary, "Settings", theme::apply_secondary, HOME_INTENT(onSettingsPressed));
  for (lv_obj_t *b : {ui.btn_profiles, ui.btn_calibrate, ui.btn_settings}) {
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_height(b, lv_pct(100));
    // Clickable only with a healthy link; DISABLED also greys it (§14 / §9 gate) — same as the
    // tiles.
    lv_obj_bind_flag_if_eq(b, &subj_link_state, LV_OBJ_FLAG_CLICKABLE, LINK_OK);
    lv_obj_bind_state_if_not_eq(b, &subj_link_state, LV_STATE_DISABLED, LINK_OK);
  }

  return ui;
}
