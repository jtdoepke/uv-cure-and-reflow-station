#include "numeric_keypad.h"

#include <cstdint>
#include <cstdio>

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
void on_caution_changed(lv_observer_t *observer, lv_subject_t *subject) {
  lv_obj_t *label = lv_observer_get_target_obj(observer);
  auto *vm = static_cast<NumericKeypadViewModel *>(lv_observer_get_user_data(observer));
  if (vm->config().cautionActive(lv_subject_get_int(subject))) {
    lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
  }
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

  theme::apply_screen(parent);
  lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_all(parent, theme::PAD_S, 0);
  lv_obj_set_style_pad_column(parent, theme::GAP, 0);

  // --- Left: the 3×4 digit grid (fills the width the rail leaves). ---
  lv_obj_t *grid = lv_obj_create(parent);
  theme::apply_row(grid);
  lv_obj_set_flex_grow(grid, 1);
  lv_obj_set_height(grid, lv_pct(100));
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

  // --- Right rail: field name, live value, range, caution, ✕ Cancel. ---
  lv_obj_t *rail = lv_obj_create(parent);
  theme::apply_panel(rail);
  lv_obj_set_width(rail, theme::KEYPAD_RAIL_W);
  lv_obj_set_height(rail, lv_pct(100));
  lv_obj_set_flex_flow(rail, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(rail, theme::PAD_S, 0);

  ui.title_label = lv_label_create(rail);
  lv_label_set_text(ui.title_label, title);
  lv_obj_set_width(ui.title_label, lv_pct(100));
  lv_label_set_long_mode(ui.title_label, LV_LABEL_LONG_WRAP);

  // Big value + units; wraps if "<value> <units>" overruns the narrow rail. Colour tracks valid.
  ui.value_label = lv_label_create(rail);
  lv_obj_set_width(ui.value_label, lv_pct(100));
  lv_obj_set_style_text_font(ui.value_label, &red_hat_mono_28, 0);
  lv_label_set_long_mode(ui.value_label, LV_LABEL_LONG_WRAP);
  lv_subject_add_observer_obj(value, on_value_changed, ui.value_label, &vm);
  lv_subject_add_observer_obj(valid, on_value_color, ui.value_label, &vm);

  // Range: always shown, dim (§26). ASCII hyphen — the font carries 0x20-0x7F + °.
  const NumericFieldConfig &cfg = vm.config();
  ui.range_label = lv_label_create(rail);
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

  // Amber caution, shown only above default; grows to push Cancel to the rail's foot.
  ui.caution_label = lv_label_create(rail);
  lv_label_set_text(ui.caution_label, cfg.caution != nullptr ? cfg.caution : "");
  lv_obj_set_style_text_color(ui.caution_label, theme::col(theme::WARN), 0);
  lv_obj_set_width(ui.caution_label, lv_pct(100));
  lv_obj_set_flex_grow(ui.caution_label, 1);
  lv_label_set_long_mode(ui.caution_label, LV_LABEL_LONG_WRAP);
  lv_obj_add_flag(ui.caution_label, LV_OBJ_FLAG_HIDDEN);
  lv_subject_add_observer_obj(value, on_caution_changed, ui.caution_label, &vm);

  // ✕ Cancel — discard, return the old value (§26). Big glyph, clears the 56 px floor.
  ui.btn_cancel = lv_button_create(rail);
  theme::apply_keypad_key(ui.btn_cancel);
  lv_obj_set_width(ui.btn_cancel, lv_pct(100));
  lv_obj_set_height(ui.btn_cancel, theme::TOUCH_MIN);
  lv_obj_t *cancel_label = lv_label_create(ui.btn_cancel);
  lv_label_set_text(cancel_label, LV_SYMBOL_CLOSE);
  lv_obj_center(cancel_label);
  lv_obj_add_event_cb(ui.btn_cancel, KEYPAD_INTENT(onCancel), LV_EVENT_CLICKED, &vm);

  return ui;
}
