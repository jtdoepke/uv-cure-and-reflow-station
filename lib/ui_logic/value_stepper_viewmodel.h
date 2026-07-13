// ValueStepperViewModel — the logic layer behind the value-stepper editor (§24, C2).
//
// Unlike HomeViewModel (a stateless global singleton over the shared subjects), each editor
// holds transient per-edit state — the working value and the field's bounds — so the view
// model is **instance-owned**: the caller constructs one per editor (stack or member), the view
// binds to its subject, and it dies with the editor. This is the reusable, config-driven shape
// the profile editor (§12, C5) and the Settings panels (§24, C8) will reuse.
//
// The maths lives in the pure NumericFieldConfig (app_logic); this class only turns intents into
// subject writes and exposes the integration seams (keypad + commit/cancel) as plain C function
// pointers so C1/C8 can plug in later without this widget depending on them.
#pragma once

#include <lvgl.h>

#include "numeric_field.h"

class ValueStepperViewModel {
public:
  // Prepare the model for a fresh edit. Call after lv_init() and before building the view
  // (same ordering discipline as ui_subjects_init()); re-init-safe so a caller may reuse the
  // instance. `initial` is clamped into the field's range. Seams are cleared — (re)install them
  // after init().
  void init(const NumericFieldConfig &config, int32_t initial);

  // Stepper intents — nudge the working value one step, saturating at the bounds. Wired to both
  // LV_EVENT_CLICKED and LV_EVENT_LONG_PRESSED_REPEAT (press-and-hold accelerates, §24).
  void onMinus();
  void onPlus();

  // Tap-the-value seam (§24 → §26 keypad, C1): invokes the value-tap handler if one is set.
  void onValueTapped();

  // Footer intents. Save invokes the commit handler with the current value; Cancel restores the
  // value the editor opened with and invokes the cancel handler. Neither starts heat/UV — the
  // §19 Confirm is the real energizing gate (§24).
  void onSave();
  void onCancel();

  // Integration seams (default: none). C-style function pointers, not std::function, to stay
  // allocation-free and match the codebase's captureless-callback idiom.
  void setValueTapHandler(void (*cb)(void *user_data), void *user_data);
  void setCommitHandler(void (*cb)(int32_t value, void *user_data), void *user_data);
  void setCancelHandler(void (*cb)(void *user_data), void *user_data);

  // Non-const: LVGL's lv_subject_get_int takes a non-const subject pointer.
  int32_t value() { return lv_subject_get_int(&value_subject_); }
  const NumericFieldConfig &config() const { return config_; }
  lv_subject_t *valueSubject() { return &value_subject_; }

private:
  NumericFieldConfig config_{};
  int32_t initial_ = 0;
  lv_subject_t value_subject_{};

  void (*on_value_tap_)(void *) = nullptr;
  void *value_tap_ud_ = nullptr;
  void (*on_commit_)(int32_t, void *) = nullptr;
  void *commit_ud_ = nullptr;
  void (*on_cancel_)(void *) = nullptr;
  void *cancel_ud_ = nullptr;
};
