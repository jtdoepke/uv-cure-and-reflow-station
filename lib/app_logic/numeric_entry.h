// NumericEntry — the pure digit-accumulation logic behind the on-screen keypad (§26, C1).
//
// The keypad is the primary editor for every wide-range numeric field (the >20-step rule,
// §24): temp caps, phase target/ramp/hold, exposure. This class owns the *typing* state a
// NumericFieldConfig does not — the digits entered so far — and enforces §26's "constrained,
// not validated-after" rule: a digit that would push the value over `max` is refused outright,
// and `OK` is disabled until the value lands in [min, max].
//
// Pure C++: no LVGL, no Arduino — the range/edge rules are exactly the off-device logic §26
// calls out to unit-test. Lives in app_logic beside NumericFieldConfig (whose bounds/format it
// reuses) and is host-tested under native_logic_cyd; the LVGL keypad view (lib/ui_logic) binds
// to a thin view model wrapping this.
#pragma once

#include <cstddef>
#include <cstdint>

#include "numeric_field.h"

// Integer-only (§26 — no decimal point, no sign). Holds the working value plus how many digits
// have been entered (0 = empty), so "empty" is distinct from a typed "0" and backspace is exact.
class NumericEntry {
public:
  // Pre-load for a fresh edit (§26: "opens pre-loaded with the current value"). `initial` is
  // clamped into the field's range; its digits count as already-entered, so backspace walks back
  // through them. Re-init-safe — a caller may reuse the instance.
  void reset(const NumericFieldConfig &config, int32_t initial) {
    config_ = config;
    value_ = config_.clamp(initial);
    digits_ = digitCount(value_);
  }

  // Append one keypad digit. Refuses (returns false, no state change) a non-0..9 input or a digit
  // that would carry the value past `max` — the over-max block (§26: you can't type an
  // out-of-range number). A leading digit replaces the empty state rather than shifting a zero.
  bool appendDigit(int d) {
    if (d < 0 || d > 9) {
      return false;
    }
    int64_t candidate = isEmpty() ? d : static_cast<int64_t>(value_) * 10 + d;
    if (candidate > config_.max) {
      return false;
    }
    value_ = static_cast<int32_t>(candidate);
    ++digits_;
    return true;
  }

  // Delete the last digit (§26 ⌫); no-op when already empty. Dropping the final digit returns to
  // the empty state, not a typed "0".
  void backspace() {
    if (isEmpty()) {
      return;
    }
    --digits_;
    value_ = isEmpty() ? 0 : value_ / 10;
  }

  // Empty the value (§26: long-press ⌫).
  void clear() {
    value_ = 0;
    digits_ = 0;
  }

  bool isEmpty() const { return digits_ == 0; }

  // The typed value; 0 when empty (callers gate on isEmpty()/okEnabled() before committing).
  int32_t value() const { return value_; }

  // Whether the current value may be committed (§26: OK disabled until in [min, max]). Empty and
  // still-below-min both hold OK disabled; the over-max side is already unreachable via typing.
  bool okEnabled() const { return !isEmpty() && value_ >= config_.min && value_ <= config_.max; }

  const NumericFieldConfig &config() const { return config_; }

  // Render the current value for the readout, reusing the field's "<value> <units>" format.
  // Empty shows nothing (an empty string), so the field name/range still frame a blank entry.
  int format(char *buf, size_t n) const {
    if (isEmpty()) {
      if (n > 0) {
        buf[0] = '\0';
      }
      return 0;
    }
    return config_.format(value_, buf, n);
  }

private:
  // Decimal digit count of a non-negative value (used to seed digits_ from a pre-loaded value).
  // 0 has one digit, matching how a typed "0" would count.
  static int digitCount(int32_t v) {
    int n = 1;
    for (int32_t t = v; t >= 10; t /= 10) {
      ++n;
    }
    return n;
  }

  NumericFieldConfig config_{};
  int32_t value_ = 0;
  int digits_ = 0; // number of entered digits; 0 = empty
};
