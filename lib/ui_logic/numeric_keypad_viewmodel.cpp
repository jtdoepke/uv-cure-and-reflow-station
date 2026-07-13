#include "numeric_keypad_viewmodel.h"

void NumericKeypadViewModel::init(const NumericFieldConfig &config, int32_t initial) {
  entry_.reset(config, initial);
  // Re-init-safe: lv_subject_init_int resets each subject to a fresh int subject.
  lv_subject_init_int(&value_subject_, entry_.value());
  lv_subject_init_int(&valid_subject_, entry_.okEnabled() ? 1 : 0);
  // A fresh edit starts with no seams; the caller (re)installs them after init().
  on_commit_ = nullptr;
  on_cancel_ = nullptr;
  commit_ud_ = cancel_ud_ = nullptr;
}

void NumericKeypadViewModel::onDigit(int d) {
  entry_.appendDigit(d); // over-max digits are refused inside NumericEntry (no-op here)
  publish();
}

void NumericKeypadViewModel::onBackspace() {
  entry_.backspace();
  publish();
}

void NumericKeypadViewModel::onClear() {
  entry_.clear();
  publish();
}

void NumericKeypadViewModel::onOk() {
  if (!entry_.okEnabled()) {
    return; // OK is disabled out of range (§26); guard the seam too
  }
  if (on_commit_ != nullptr) {
    on_commit_(entry_.value(), commit_ud_);
  }
}

void NumericKeypadViewModel::onCancel() {
  // Nothing to restore — the keypad edits a working copy and never wrote the field. Just report.
  if (on_cancel_ != nullptr) {
    on_cancel_(cancel_ud_);
  }
}

void NumericKeypadViewModel::setCommitHandler(void (*cb)(int32_t, void *), void *user_data) {
  on_commit_ = cb;
  commit_ud_ = user_data;
}

void NumericKeypadViewModel::setCancelHandler(void (*cb)(void *), void *user_data) {
  on_cancel_ = cb;
  cancel_ud_ = user_data;
}

void NumericKeypadViewModel::publish() {
  lv_subject_set_int(&value_subject_, entry_.value());
  lv_subject_set_int(&valid_subject_, entry_.okEnabled() ? 1 : 0);
}
