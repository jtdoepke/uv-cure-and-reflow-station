// NumericKeypadViewModel — the logic layer behind the on-screen keypad editor (§26, C1).
//
// Like ValueStepperViewModel (§24, C2), it is **instance-owned**: the caller constructs one per
// edit, the view binds to its subjects, and it dies with the editor. This is the shared,
// config-driven shape the profile editor (§12, C5) and Settings panels (§24, C8) reuse — both
// numeric editors are driven by the same per-field NumericFieldConfig.
//
// The typing maths lives in the pure NumericEntry (app_logic); this class turns key intents into
// subject writes and exposes the commit/cancel seams as captureless C function pointers so those
// callers can plug in later without this widget depending on them. Two subjects, per §26: the
// typed value and a valid flag (OK-enabled) the view reads to gate OK and colour the readout.
#pragma once

#include <lvgl.h>

#include "numeric_entry.h"
#include "numeric_field.h"

class NumericKeypadViewModel {
public:
  // Prepare the model for a fresh edit. Call after lv_init() and before building the view (same
  // ordering discipline as the stepper); re-init-safe so a caller may reuse the instance.
  // `initial` pre-loads the value (§26 opens on the current value); it is clamped into range.
  // Seams are cleared — (re)install them after init().
  void init(const NumericFieldConfig &config, int32_t initial);

  // Key intents. Digits append (over-max digits are refused by NumericEntry); ⌫ deletes the last
  // digit; long-press ⌫ empties (§26). Each republishes the value + valid subjects.
  void onDigit(int d);
  void onBackspace();
  void onClear();

  // Footer intents. OK commits the typed value back to the field — but only when it is in range
  // (§26: OK is disabled otherwise, and this is the belt-and-braces guard). Cancel discards and
  // invokes the cancel handler with the field's old value untouched. Neither talks to the
  // controller — the keypad just edits a number (§26).
  void onOk();
  void onCancel();

  // Integration seams (default: none). C-style function pointers, not std::function, to stay
  // allocation-free and match the codebase's captureless-callback idiom (as the stepper does).
  void setCommitHandler(void (*cb)(int32_t value, void *user_data), void *user_data);
  void setCancelHandler(void (*cb)(void *user_data), void *user_data);

  // Non-const: lv_subject_get_int takes a non-const subject pointer.
  int32_t value() { return lv_subject_get_int(&value_subject_); }
  bool valid() { return lv_subject_get_int(&valid_subject_) != 0; }
  const NumericFieldConfig &config() const { return entry_.config(); }
  NumericEntry &entry() { return entry_; }
  lv_subject_t *valueSubject() { return &value_subject_; }
  lv_subject_t *validSubject() { return &valid_subject_; }

private:
  // Push the working NumericEntry state out to the two subjects (view updates via observers).
  void publish();

  NumericEntry entry_{};
  lv_subject_t value_subject_{};
  lv_subject_t valid_subject_{};

  void (*on_commit_)(int32_t, void *) = nullptr;
  void *commit_ud_ = nullptr;
  void (*on_cancel_)(void *) = nullptr;
  void *cancel_ud_ = nullptr;
};
