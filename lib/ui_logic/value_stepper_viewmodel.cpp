#include "value_stepper_viewmodel.h"

void ValueStepperViewModel::init(const NumericFieldConfig &config, int32_t initial) {
  config_ = config;
  initial_ = config_.clamp(initial);
  // Re-init-safe: lv_subject_init_int resets the subject to a fresh int subject each time.
  lv_subject_init_int(&value_subject_, initial_);
  // A fresh edit starts with no seams; the caller (re)installs them after init().
  on_value_tap_ = nullptr;
  on_commit_ = nullptr;
  on_cancel_ = nullptr;
  value_tap_ud_ = commit_ud_ = cancel_ud_ = nullptr;
}

void ValueStepperViewModel::onMinus() {
  lv_subject_set_int(&value_subject_, config_.stepDown(value()));
}

void ValueStepperViewModel::onPlus() {
  lv_subject_set_int(&value_subject_, config_.stepUp(value()));
}

void ValueStepperViewModel::onValueTapped() {
  if (on_value_tap_ != nullptr) {
    on_value_tap_(value_tap_ud_);
  }
}

void ValueStepperViewModel::onSave() {
  if (on_commit_ != nullptr) {
    on_commit_(value(), commit_ud_);
  }
}

void ValueStepperViewModel::onCancel() {
  lv_subject_set_int(&value_subject_, initial_); // discard edits, restore the opening value
  if (on_cancel_ != nullptr) {
    on_cancel_(cancel_ud_);
  }
}

void ValueStepperViewModel::setValueTapHandler(void (*cb)(void *), void *user_data) {
  on_value_tap_ = cb;
  value_tap_ud_ = user_data;
}

void ValueStepperViewModel::setCommitHandler(void (*cb)(int32_t, void *), void *user_data) {
  on_commit_ = cb;
  commit_ud_ = user_data;
}

void ValueStepperViewModel::setCancelHandler(void (*cb)(void *), void *user_data) {
  on_cancel_ = cb;
  cancel_ud_ = user_data;
}
