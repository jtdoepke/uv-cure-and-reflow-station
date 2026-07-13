#include "value_stepper.h"

#include <cstdio>

#include "theme.h"

namespace {

// --- Observers: working value → view. ---

void on_value_changed(lv_observer_t *observer, lv_subject_t *subject) {
  lv_obj_t *label = lv_observer_get_target_obj(observer);
  auto *vm = static_cast<ValueStepperViewModel *>(lv_observer_get_user_data(observer));
  char buf[24];
  vm->config().format(lv_subject_get_int(subject), buf, sizeof(buf));
  lv_label_set_text(label, buf);
}

// −/+ disable at the range ends ("disable, don't hide", §24). Each observer owns one button and
// reads its at-limit predicate from the field config.
void on_minus_state(lv_observer_t *observer, lv_subject_t *subject) {
  lv_obj_t *btn = lv_observer_get_target_obj(observer);
  auto *vm = static_cast<ValueStepperViewModel *>(lv_observer_get_user_data(observer));
  lv_obj_set_state(btn, LV_STATE_DISABLED, vm->config().atMin(lv_subject_get_int(subject)));
}

void on_plus_state(lv_observer_t *observer, lv_subject_t *subject) {
  lv_obj_t *btn = lv_observer_get_target_obj(observer);
  auto *vm = static_cast<ValueStepperViewModel *>(lv_observer_get_user_data(observer));
  lv_obj_set_state(btn, LV_STATE_DISABLED, vm->config().atMax(lv_subject_get_int(subject)));
}

// Amber caution shown only while the value sits above the field default (§24).
void on_caution_changed(lv_observer_t *observer, lv_subject_t *subject) {
  lv_obj_t *label = lv_observer_get_target_obj(observer);
  auto *vm = static_cast<ValueStepperViewModel *>(lv_observer_get_user_data(observer));
  bool show = vm->config().cautionActive(lv_subject_get_int(subject));
  if (show) {
    lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
  }
}

// The shared thunk body: recover the view model from user_data and invoke one of its intents.
// (An lv_event_cb_t can't be a member function.)
#define STEPPER_INTENT(method)                                                                     \
  [](lv_event_t *e) { static_cast<ValueStepperViewModel *>(lv_event_get_user_data(e))->method(); }

} // namespace

ValueStepper create_value_stepper(lv_obj_t *parent, ValueStepperViewModel &vm, const char *title) {
  ValueStepper ui{};
  ui.root = parent;
  lv_subject_t *value = vm.valueSubject();

  theme::apply_screen(parent);
  lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(parent, theme::PAD_S, 0);
  lv_obj_set_style_pad_row(parent, theme::GAP, 0);

  // --- Header: the field being edited ---
  lv_obj_t *header = lv_obj_create(parent);
  theme::apply_panel(header);
  lv_obj_set_width(header, lv_pct(100));
  lv_obj_set_height(header, theme::HEADER_H);
  lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  ui.title_label = lv_label_create(header);
  lv_label_set_text(ui.title_label, title);

  // --- Stepper row: [ − ]  value  [ + ] — fills the vertical middle so the buttons are big ---
  lv_obj_t *row = lv_obj_create(parent);
  theme::apply_row(row);
  lv_obj_set_width(row, lv_pct(100));
  lv_obj_set_flex_grow(row, 1);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  ui.btn_minus = lv_button_create(row);
  theme::apply_stepper_button(ui.btn_minus);
  lv_obj_t *minus_label = lv_label_create(ui.btn_minus);
  lv_label_set_text(minus_label, "-");
  lv_obj_center(minus_label);
  lv_obj_add_event_cb(ui.btn_minus, STEPPER_INTENT(onMinus), LV_EVENT_CLICKED, &vm);
  // Press-and-hold accelerates: LVGL re-fires this while the button stays held (§24).
  lv_obj_add_event_cb(ui.btn_minus, STEPPER_INTENT(onMinus), LV_EVENT_LONG_PRESSED_REPEAT, &vm);
  lv_subject_add_observer_obj(value, on_minus_state, ui.btn_minus, &vm);

  // Big, centred value+units; tapping it opens the keypad (§26) for direct entry.
  ui.value_label = lv_label_create(row);
  lv_obj_set_flex_grow(ui.value_label, 1);
  lv_obj_set_style_text_font(ui.value_label, &red_hat_mono_28, 0);
  lv_obj_set_style_text_align(ui.value_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_add_flag(ui.value_label, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(ui.value_label, STEPPER_INTENT(onValueTapped), LV_EVENT_CLICKED, &vm);
  lv_subject_add_observer_obj(value, on_value_changed, ui.value_label, &vm);

  ui.btn_plus = lv_button_create(row);
  theme::apply_stepper_button(ui.btn_plus);
  lv_obj_t *plus_label = lv_label_create(ui.btn_plus);
  lv_label_set_text(plus_label, "+");
  lv_obj_center(plus_label);
  lv_obj_add_event_cb(ui.btn_plus, STEPPER_INTENT(onPlus), LV_EVENT_CLICKED, &vm);
  lv_obj_add_event_cb(ui.btn_plus, STEPPER_INTENT(onPlus), LV_EVENT_LONG_PRESSED_REPEAT, &vm);
  lv_subject_add_observer_obj(value, on_plus_state, ui.btn_plus, &vm);

  // --- Context: range + default (ASCII only — the font carries 0x20-0x7F + °) ---
  const NumericFieldConfig &cfg = vm.config();
  ui.range_label = lv_label_create(parent);
  lv_obj_set_style_text_color(ui.range_label, theme::col(theme::TEXT_DIM), 0);
  char range_buf[48];
  if (cfg.units != nullptr && cfg.units[0] != '\0') {
    std::snprintf(range_buf, sizeof(range_buf), "Range %d-%d, default %d %s", cfg.min, cfg.max,
                  cfg.defaultValue, cfg.units);
  } else {
    std::snprintf(range_buf, sizeof(range_buf), "Range %d-%d, default %d", cfg.min, cfg.max,
                  cfg.defaultValue);
  }
  lv_label_set_text(ui.range_label, range_buf);

  // --- Amber caution: shown only above default ---
  ui.caution_label = lv_label_create(parent);
  lv_label_set_text(ui.caution_label, cfg.caution != nullptr ? cfg.caution : "");
  lv_obj_set_style_text_color(ui.caution_label, theme::col(theme::WARN), 0);
  lv_obj_set_width(ui.caution_label, lv_pct(100));
  lv_label_set_long_mode(ui.caution_label, LV_LABEL_LONG_WRAP);
  lv_obj_add_flag(ui.caution_label, LV_OBJ_FLAG_HIDDEN);
  lv_subject_add_observer_obj(value, on_caution_changed, ui.caution_label, &vm);

  // --- Footer: Cancel (discard) / Save (commit). Plain buttons — editing starts no heat (§24). ---
  lv_obj_t *footer = lv_obj_create(parent);
  theme::apply_row(footer);
  lv_obj_set_width(footer, lv_pct(100));
  lv_obj_set_height(footer, theme::SECONDARY_H);
  lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_ROW);

  ui.btn_cancel = lv_button_create(footer);
  theme::apply_secondary(ui.btn_cancel);
  lv_obj_set_flex_grow(ui.btn_cancel, 1);
  lv_obj_set_height(ui.btn_cancel, lv_pct(100));
  lv_obj_t *cancel_label = lv_label_create(ui.btn_cancel);
  lv_label_set_text(cancel_label, "Cancel");
  lv_obj_center(cancel_label);
  lv_obj_add_event_cb(ui.btn_cancel, STEPPER_INTENT(onCancel), LV_EVENT_CLICKED, &vm);

  ui.btn_save = lv_button_create(footer);
  theme::apply_secondary(ui.btn_save);
  lv_obj_set_flex_grow(ui.btn_save, 1);
  lv_obj_set_height(ui.btn_save, lv_pct(100));
  lv_obj_t *save_label = lv_label_create(ui.btn_save);
  lv_label_set_text(save_label, LV_SYMBOL_OK " Save");
  lv_obj_center(save_label);
  lv_obj_add_event_cb(ui.btn_save, STEPPER_INTENT(onSave), LV_EVENT_CLICKED, &vm);

  return ui;
}
