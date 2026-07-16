#include "numeric_keypad.h"

#include <cstdint>
#include <cstdio>

#include "panel.h" // geometry (portrait vs landscape) — never a board identity
#include "theme.h"

namespace {

// --- Observers: working state → view. ---

// Value readout. Formats from the live NumericEntry (not the raw subject int) so the empty state
// renders as a blank rather than a typed "0"; the subject is the trigger.
void on_value_changed(lv_observer_t *observer, lv_subject_t *) {
  lv_obj_t *label = lv_observer_get_target_obj(observer);
  auto *vm = static_cast<NumericKeypadViewModel *>(lv_observer_get_user_data(observer));
  char buf[24];
  vm->entry().format(buf, sizeof(buf));
  lv_label_set_text(label, buf);
}

// The value goes amber while OK is disabled (§26: "amber with the range reminder while OK is
// disabled — you always see why"). Bound to the valid flag.
void on_value_color(lv_observer_t *observer, lv_subject_t *subject) {
  lv_obj_t *label = lv_observer_get_target_obj(observer);
  bool valid = lv_subject_get_int(subject) != 0;
  lv_obj_set_style_text_color(label, theme::col(valid ? theme::TEXT : theme::WARN), 0);
}

// OK disables until the value is in range ("disable, don't hide", §24/§26). Bound to the valid
// flag; the intent guards again so a stray event out of range is still a no-op.
void on_ok_state(lv_observer_t *observer, lv_subject_t *subject) {
  lv_obj_t *btn = lv_observer_get_target_obj(observer);
  lv_obj_set_state(btn, LV_STATE_DISABLED, lv_subject_get_int(subject) == 0);
}

// Amber caution shown only while the typed value sits above the field default (§24/§26) — for a
// temp cap this is the honest per-mode note in the rail. Bound to the value.
//
// Shown/hidden by TEXT OPACITY, not LV_OBJ_FLAG_HIDDEN, so the label keeps its place in the flex
// layout at all times and appearing costs nothing around it. LVGL drops hidden children from flex
// entirely, so the flag made the rail reflow mid-typing and slid Cancel out from under the finger
// reaching for it — 38 px in landscape, 12 px in portrait (test_keypad pins both). The label's
// text is fixed at creation, so the space it reserves never changes size either.
void on_caution_changed(lv_observer_t *observer, lv_subject_t *subject) {
  lv_obj_t *label = lv_observer_get_target_obj(observer);
  auto *vm = static_cast<NumericKeypadViewModel *>(lv_observer_get_user_data(observer));
  const bool active = vm->config().cautionActive(lv_subject_get_int(subject));
  lv_obj_set_style_text_opa(label, active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

// Event → intent thunk for the parameterless control keys (an lv_event_cb_t can't be a member).
#define KEYPAD_INTENT(method)                                                                      \
  [](lv_event_t *e) { static_cast<NumericKeypadViewModel *>(lv_event_get_user_data(e))->method(); }

// Digit keys carry their digit in the widget's user_data and the view model in the event's; this
// one handler serves all ten.
void on_digit_clicked(lv_event_t *e) {
  auto *vm = static_cast<NumericKeypadViewModel *>(lv_event_get_user_data(e));
  lv_obj_t *btn = lv_event_get_target_obj(e);
  int d = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(btn)));
  vm->onDigit(d);
}

// Build one key button inside `row`: equal-width, full-row-height, big-glyph, centred label.
lv_obj_t *make_key(lv_obj_t *row, const char *text) {
  lv_obj_t *btn = lv_button_create(row);
  theme::apply_keypad_key(btn);
  lv_obj_set_flex_grow(btn, 1);
  lv_obj_set_height(btn, lv_pct(100));
  lv_obj_t *label = lv_label_create(btn);
  lv_label_set_text(label, text);
  lv_obj_center(label);
  return btn;
}

// A grid row: transparent flex row filling the vertical share, three keys wide.
lv_obj_t *make_row(lv_obj_t *col) {
  lv_obj_t *row = lv_obj_create(col);
  theme::apply_row(row);
  lv_obj_set_width(row, lv_pct(100));
  lv_obj_set_flex_grow(row, 1);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_gap(row, theme::PAD_S, 0);
  return row;
}

} // namespace

NumericKeypad create_numeric_keypad(lv_obj_t *parent, NumericKeypadViewModel &vm,
                                    const char *title) {
  NumericKeypad ui{};
  ui.root = parent;
  lv_subject_t *value = vm.valueSubject();
  lv_subject_t *valid = vm.validSubject();

  // Landscape puts the rail down the right-hand side and the grid left of it; portrait lays the
  // rail across the top with the grid beneath. Gated on panel::kPortrait — the GEOMETRY — never a
  // board flag. A side rail is a landscape idea: on a 320 px-wide panel it leaves ~111 px, which
  // wraps "Target temp" onto two lines, wraps the big "100 °C" readout, and still strands empty
  // space under Cancel — all while squeezing the keys it was supposed to sit beside.
  //
  // Both orientations keep the SAME widget tree (rail > info + cancel); only flow and the
  // main/cross sizes flip. That is the point: one branch per screen, so there is one keypad to
  // reason about and the view model, observers and tests cannot tell the two apart.
  const bool portrait = panel::kPortrait;

  theme::apply_screen(parent);
  lv_obj_set_flex_flow(parent, portrait ? LV_FLEX_FLOW_COLUMN : LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_all(parent, theme::PAD_S, 0);
  lv_obj_set_style_pad_column(parent, theme::GAP, 0);
  lv_obj_set_style_pad_row(parent, theme::GAP, 0);

  // --- The 3×4 digit grid (fills whatever the rail leaves). ---
  lv_obj_t *grid = lv_obj_create(parent);
  theme::apply_row(grid);
  lv_obj_set_flex_grow(grid, 1);
  if (portrait) {
    lv_obj_set_width(grid, lv_pct(100));
  } else {
    lv_obj_set_height(grid, lv_pct(100));
  }
  lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(grid, theme::PAD_S, 0);

  // Rows 1-2-3 / 4-5-6 / 7-8-9 in the universal keypad order (§26).
  static const char *const kDigitRows[3][3] = {{"1", "2", "3"}, {"4", "5", "6"}, {"7", "8", "9"}};
  for (auto &digits : kDigitRows) {
    lv_obj_t *row = make_row(grid);
    for (const char *d : digits) {
      lv_obj_t *btn = make_key(row, d);
      int digit = d[0] - '0';
      lv_obj_set_user_data(btn, reinterpret_cast<void *>(static_cast<intptr_t>(digit)));
      lv_obj_add_event_cb(btn, on_digit_clicked, LV_EVENT_CLICKED, &vm);
      ui.keys[digit] = btn;
    }
  }

  // Bottom row: ⌫ · 0 · OK ✓.
  lv_obj_t *last = make_row(grid);
  ui.btn_backspace = make_key(last, LV_SYMBOL_BACKSPACE);
  lv_obj_add_event_cb(ui.btn_backspace, KEYPAD_INTENT(onBackspace), LV_EVENT_CLICKED, &vm);
  // Long-press ⌫ empties the value (§26 — Clear is dropped in favour of this).
  lv_obj_add_event_cb(ui.btn_backspace, KEYPAD_INTENT(onClear), LV_EVENT_LONG_PRESSED, &vm);

  ui.keys[0] = make_key(last, "0");
  lv_obj_set_user_data(ui.keys[0], reinterpret_cast<void *>(static_cast<intptr_t>(0)));
  lv_obj_add_event_cb(ui.keys[0], on_digit_clicked, LV_EVENT_CLICKED, &vm);

  ui.btn_ok = make_key(last, LV_SYMBOL_OK);
  lv_obj_add_event_cb(ui.btn_ok, KEYPAD_INTENT(onOk), LV_EVENT_CLICKED, &vm);
  lv_subject_add_observer_obj(valid, on_ok_state, ui.btn_ok, &vm);

  // --- Rail: field name, live value, range, caution, ✕ Cancel. Right side in landscape, a strip
  // across the top in portrait (created after the grid either way, then moved into place). ---
  lv_obj_t *rail = lv_obj_create(parent);
  theme::apply_panel(rail);
  lv_obj_set_flex_flow(rail, portrait ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(rail, theme::PAD_S, 0);
  lv_obj_set_style_pad_column(rail, theme::PAD_S, 0);
  if (portrait) {
    lv_obj_set_width(rail, lv_pct(100));
    lv_obj_set_height(rail, LV_SIZE_CONTENT); // only as tall as the text needs — the grid gets rest
    lv_obj_set_flex_align(rail, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_move_to_index(rail, 0); // flex order is creation order; the strip belongs on top
  } else {
    lv_obj_set_width(rail, theme::KEYPAD_RAIL_W);
    lv_obj_set_height(rail, lv_pct(100));
  }

  // The text block. Carries the flex-grow that pushes Cancel to the rail's far end — the foot of
  // the column in landscape, the right-hand end of the strip in portrait. Previously the caution
  // label held that grow, which only worked while the rail was a column.
  lv_obj_t *info = lv_obj_create(rail);
  theme::apply_row(info);
  lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_grow(info,
                       1); // takes the rail's main axis: width in portrait, height in landscape
  lv_obj_set_style_pad_row(info, theme::PAD_S, 0);
  // The CROSS axis must be pinned explicitly. Leaving it at the default size-to-content makes the
  // children's lv_pct(100) widths resolve against a width derived from those same children, and
  // the labels then render past the rail's edge instead of wrapping inside it.
  if (portrait) {
    lv_obj_set_height(info, LV_SIZE_CONTENT);
  } else {
    lv_obj_set_width(info, lv_pct(100));
  }

  ui.title_label = lv_label_create(info);
  lv_label_set_text(ui.title_label, title);
  lv_obj_set_width(ui.title_label, lv_pct(100));
  lv_label_set_long_mode(ui.title_label, LV_LABEL_LONG_WRAP);

  // Big value + units; wraps if "<value> <units>" overruns the narrow rail. Colour tracks valid.
  ui.value_label = lv_label_create(info);
  lv_obj_set_width(ui.value_label, lv_pct(100));
  lv_obj_set_style_text_font(ui.value_label, &theme::big_font(), 0);
  lv_label_set_long_mode(ui.value_label, LV_LABEL_LONG_WRAP);
  lv_subject_add_observer_obj(value, on_value_changed, ui.value_label, &vm);
  lv_subject_add_observer_obj(valid, on_value_color, ui.value_label, &vm);

  // Range: always shown, dim (§26). ASCII hyphen — the font carries 0x20-0x7F + °.
  const NumericFieldConfig &cfg = vm.config();
  ui.range_label = lv_label_create(info);
  lv_obj_set_width(ui.range_label, lv_pct(100));
  lv_label_set_long_mode(ui.range_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_color(ui.range_label, theme::col(theme::TEXT_DIM), 0);
  char range_buf[32];
  if (cfg.units != nullptr && cfg.units[0] != '\0') {
    std::snprintf(range_buf, sizeof(range_buf), "%d-%d %s", cfg.min, cfg.max, cfg.units);
  } else {
    std::snprintf(range_buf, sizeof(range_buf), "%d-%d", cfg.min, cfg.max);
  }
  lv_label_set_text(ui.range_label, range_buf);

  // Amber caution, revealed only above default. It always holds its slot in the layout (see
  // on_caution_changed) — the space is reserved from creation, so nothing shifts when it appears.
  ui.caution_label = lv_label_create(info);
  lv_label_set_text(ui.caution_label, cfg.caution != nullptr ? cfg.caution : "");
  lv_obj_set_style_text_color(ui.caution_label, theme::col(theme::WARN), 0);
  lv_obj_set_width(ui.caution_label, lv_pct(100));
  lv_label_set_long_mode(ui.caution_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_opa(ui.caution_label, LV_OPA_TRANSP, 0);
  lv_subject_add_observer_obj(value, on_caution_changed, ui.caution_label, &vm);

  // ✕ Cancel — discard, return the old value (§26). Big glyph, clears the 56 px floor.
  ui.btn_cancel = lv_button_create(rail);
  theme::apply_keypad_key(ui.btn_cancel);
  lv_obj_set_width(ui.btn_cancel, portrait ? theme::TOUCH_MIN : lv_pct(100));
  lv_obj_set_height(ui.btn_cancel, theme::TOUCH_MIN);
  lv_obj_t *cancel_label = lv_label_create(ui.btn_cancel);
  lv_label_set_text(cancel_label, LV_SYMBOL_CLOSE);
  lv_obj_center(cancel_label);
  lv_obj_add_event_cb(ui.btn_cancel, KEYPAD_INTENT(onCancel), LV_EVENT_CLICKED, &vm);

  return ui;
}
