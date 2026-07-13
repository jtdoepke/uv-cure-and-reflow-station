// NumericFieldConfig — the per-field description that drives both shared numeric editors:
// the value-stepper (§24, C2) for nudge-range fields and the on-screen keypad (§26, C1) for
// wide-range fields. One config, `{min, max, step, units}`, and the >20-step rule below picks
// which editor a field gets — so the two editors never disagree about a field's bounds.
//
// Pure C++: no LVGL, no Arduino. It is the maths half of the editors (clamp / step / at-limit /
// range-classification / caution), so it lives in app_logic and is host-tested under
// native_logic_cyd, and the LVGL views (lib/ui_logic) only bind to it. C1's keypad reuses this
// same type.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

// Immutable description of one editable integer field. `units`/`caution` are borrowed string
// literals (firmware-owned, never freed). All values are whole numbers — every user-entered
// field in the design is an integer (§26); fractional quantities are derived, never typed.
struct NumericFieldConfig {
  int32_t min = 0;
  int32_t max = 0;
  int32_t step = 1;
  int32_t defaultValue = 0;
  const char *units = "";        // e.g. "min", "%", "°C" — shown after the value; may be ""
  const char *caution = nullptr; // amber note shown when value > defaultValue; nullptr = none

  // Constrain a value into [min, max].
  int32_t clamp(int32_t v) const {
    if (v < min) {
      return min;
    }
    if (v > max) {
      return max;
    }
    return v;
  }

  // One step up / down, saturating at the bounds.
  int32_t stepUp(int32_t v) const { return clamp(v + step); }
  int32_t stepDown(int32_t v) const { return clamp(v - step); }

  // Whether a (clamped) value sits at a bound — the −/+ buttons disable here ("disable, don't
  // hide", §24).
  bool atMin(int32_t v) const { return clamp(v) <= min; }
  bool atMax(int32_t v) const { return clamp(v) >= max; }

  // The >20-step rule (§24): a field whose full range costs ≤20 taps on ± gets the stepper;
  // anything wider goes straight to the keypad. `step <= 0` is treated as keypad-only (a
  // degenerate config can't be nudged sensibly). This is the routing predicate C8 calls to
  // choose an editor; C2 itself always renders a stepper.
  bool usesStepper() const { return step > 0 && (max - min) / step <= 20; }

  // Raising a field above its default is a soft warning, not a block (§24): show the caution
  // string, no confirmation friction.
  bool cautionActive(int32_t v) const { return caution != nullptr && v > defaultValue; }

  // Render "<value> <units>" (units omitted when empty). Returns the snprintf length.
  int format(int32_t v, char *buf, size_t n) const {
    if (units != nullptr && units[0] != '\0') {
      return std::snprintf(buf, n, "%d %s", static_cast<int>(v), units);
    }
    return std::snprintf(buf, n, "%d", static_cast<int>(v));
  }
};
