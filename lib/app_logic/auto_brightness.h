// AutoBrightness — phone-style automatic screen brightness for the CYD (design.md §18), and
// the single owner of the display backlight (so sleep/wake, §17, routes through it too).
//
// Portable, host-testable logic behind two narrow ports: it reads the ambient LDR through
// IAmbientLight (raw ADC counts) and drives the backlight through IBacklight, with a
// FakeAmbientLight
// + FakeBacklight under native_logic_cyd — no LovyanGFX, no Arduino. The pipeline mirrors §18:
//
//   low-pass filter (EMA)  ->  perceptual curve/LUT  ->  manual bias  ->  hysteresis  ->
//   ramp (ease, don't jump)  ->  clamp to [floor, ceiling]
//
// The floor keeps HOT / UV ON / fault legible in a dark shop and the ceiling keeps the screen
// readable in sunlight — the floor is NOT user-defeatable (bias can't push below it). Sampling
// and ramping run at a few Hz (cfg.updateIntervalMs); tick() is called every loop and is a
// cheap no-op between updates.
//
// Sleep (§17) is expressed as setAwake(false): the target collapses to 0 and the backlight
// ramps *off* smoothly. setAwake(true) wakes it **in one update, at full target level** — the
// asymmetry is deliberate (see update()): a fade-out is the screen leaving on its own, while a
// fade-in only delays the thing the operator just touched the screen to get. While asleep the
// floor does not apply (0 is intended) — a fault forces a wake first (§22), so the floor is
// always in force whenever anything must be legible.
//
// The LDR curve, floor/ceiling and filter/ramp time-constants are placeholders to be tuned on
// real glass via the ui-development device loop (design.md §10 open item).
//
// The ports are injected by reference and must outlive this object.
#pragma once

#include <cstdint>

#include "IAmbientLight.h"
#include "IBacklight.h"

class AutoBrightness {
public:
  struct Config {
    uint32_t updateIntervalMs = 250; // sample + ramp cadence (~4 Hz)
    uint8_t floorLevel = 48;     // SAFETY min while awake (HOT/UV/fault legibility); not defeatable
    uint8_t ceilLevel = 255;     // max for sunlight readability
    uint8_t rampStep = 16;       // max backlight change per update (ease, don't jump)
    uint8_t hysteresis = 6;      // min target change (levels) before the held target moves
    uint8_t manualNominal = 160; // level used when auto is OFF, before bias
    float emaAlpha = 0.25F;      // low-pass factor: smaller = smoother/slower LDR response
  };

  AutoBrightness(IAmbientLight &ldr, IBacklight &bl, Config cfg) : ldr_(ldr), bl_(bl), cfg_(cfg) {}

  // Convenience overload with default Config (a defaulted Config{} argument can't reference the
  // in-class member initializers before Config is complete — same pattern as HeaterActuator).
  AutoBrightness(IAmbientLight &ldr, IBacklight &bl) : AutoBrightness(ldr, bl, Config{}) {}

  // Pushed from settings each loop (cheap): auto on/off and the manual brightness bias (%).
  void setEnabled(bool on) { enabled_ = on; }
  void setBias(int32_t percent) { biasPct_ = percent; }

  // The level used while auto is OFF, as a percent of full scale — the whole setting on a board
  // with no light sensor, where a bias would be a trim on a constant (§18). Percent because that
  // is what the user is shown and what SettingsStore persists; levels stop at this boundary.
  //
  // The floor still applies downstream in targetLevel(), and that is deliberate: it is the
  // non-defeatable safety minimum, not this field's business to escape. SCREEN_BRIGHTNESS_MIN_PCT
  // is pitched above it so the two never fight — see settings_defaults.h.
  void setManualPercent(int32_t percent) {
    int32_t level = percent * 255 / 100;
    if (level < 0) {
      level = 0;
    }
    if (level > 255) {
      level = 255;
    }
    cfg_.manualNominal = static_cast<uint8_t>(level);
  }

  // Sleep/wake gate (§17): false => ramp the backlight off; true => resume auto brightness.
  void setAwake(bool awake) { awake_ = awake; }

  // The last level actually pushed to the backlight (for tests / diagnostics).
  uint8_t level() const { return static_cast<uint8_t>(current_); }

  // The most recent raw LDR sample (for bring-up / pin-verification logging). 0 until the first
  // enabled+awake update reads the sensor.
  uint16_t lastRaw() const { return lastRaw_; }

  // Call every loop. Runs one update (sample the LDR, recompute the target, take a ramp step,
  // push the backlight) at most once per updateIntervalMs; a no-op in between.
  void tick(uint32_t nowMs) {
    if (!started_) {
      started_ = true;
      lastUpdateMs_ = nowMs;
      update();
      return;
    }
    if (static_cast<uint32_t>(nowMs - lastUpdateMs_) < cfg_.updateIntervalMs) {
      return;
    }
    // Advance by whole intervals so a late tick doesn't drift the phase (as HeaterActuator does).
    uint32_t elapsed = static_cast<uint32_t>(nowMs - lastUpdateMs_);
    lastUpdateMs_ += (elapsed / cfg_.updateIntervalMs) * cfg_.updateIntervalMs;
    update();
  }

private:
  // {rawAdc -> brightness} breakpoints, piecewise-linear. INVERTED for the CYD's LDR wiring
  // (3.3V - R15 1M - GPIO34 - LDR||R19 - GND): bright ambient pulls the node toward 0, dark
  // lets it rise. So LOW raw = bright room = bright screen; HIGH raw = dark = dim screen, and
  // the table decreases. Calibrated on-glass to this unit: raw ~0 in room light, ~80 in a dark
  // room, ~1400 fully covered. The ESP32 ADC also floors readings below ~0.1V to 0, so there is
  // no usable signal across normal indoor light — dimming only kicks in as the room goes dark.
  // Re-tune these on your glass via the [ldr] serial trace in main.cpp's loop().
  struct CurvePoint {
    uint16_t raw;
    uint8_t level;
  };
  static constexpr CurvePoint kCurve[] = {{0, 255}, {80, 150}, {300, 90}, {800, 55}, {1400, 48}};

  static uint8_t curveLevel(float rawf) {
    int32_t raw = static_cast<int32_t>(rawf + 0.5F);
    constexpr int n = sizeof(kCurve) / sizeof(kCurve[0]);
    if (raw <= kCurve[0].raw) {
      return kCurve[0].level;
    }
    if (raw >= kCurve[n - 1].raw) {
      return kCurve[n - 1].level;
    }
    for (int i = 1; i < n; i++) {
      if (raw <= kCurve[i].raw) {
        int32_t r0 = kCurve[i - 1].raw, r1 = kCurve[i].raw;
        int32_t l0 = kCurve[i - 1].level, l1 = kCurve[i].level;
        return static_cast<uint8_t>(l0 + (l1 - l0) * (raw - r0) / (r1 - r0));
      }
    }
    return kCurve[n - 1].level;
  }

  // The awake target level: curve (or manual nominal) shifted by bias, then clamped so the
  // safety floor always wins over the bias. Asleep short-circuits to 0 (floor deliberately
  // bypassed — see the header note).
  int32_t targetLevel() {
    if (!awake_) {
      return 0;
    }
    int32_t base;
    if (enabled_) {
      // Refresh the low-pass filter from a fresh sample only while actively auto-dimming.
      uint16_t raw = ldr_.read();
      lastRaw_ = raw;
      if (!filterSeeded_) {
        filtered_ = static_cast<float>(raw);
        filterSeeded_ = true;
      } else {
        filtered_ += cfg_.emaAlpha * (static_cast<float>(raw) - filtered_);
      }
      base = curveLevel(filtered_);
    } else {
      base = cfg_.manualNominal;
    }
    // Manual bias: a +/- trim in % of full scale (255), ADDED on top of the ambient/manual level.
    // Additive rather than multiplicative so it still lifts a floored dark reading up off the
    // floor (a x1.4 of a below-floor value is still below the floor -> the trim looked dead).
    int32_t biased = base + biasPct_ * 255 / 100;
    if (biased < cfg_.floorLevel) {
      biased = cfg_.floorLevel;
    }
    if (biased > cfg_.ceilLevel) {
      biased = cfg_.ceilLevel;
    }
    return biased;
  }

  void update() {
    int32_t want = targetLevel();
    // Waking is a step, sleeping is a fade — deliberately asymmetric (§17/§18). Ramping *up* only
    // delays the thing the operator asked for: they touched a dark screen to see it, and easing
    // in makes the machine feel slow to answer. Ramping *down* is the opposite — it is the screen
    // leaving on its own, where a smooth fade reads as intent rather than as a fault, and where a
    // last glance still catches it going.
    const bool justWoke = awake_ && !wasAwake_;
    wasAwake_ = awake_;
    if (!awake_) {
      heldTarget_ = 0; // sleep must reach 0 regardless of hysteresis
    } else {
      int32_t delta = want - heldTarget_;
      if (delta < 0) {
        delta = -delta;
      }
      // Move the held target on a big-enough change, or to latch onto an exact rail.
      if (delta >= cfg_.hysteresis || want == cfg_.floorLevel || want == cfg_.ceilLevel) {
        heldTarget_ = want;
      }
    }
    // Ease toward the held target by at most rampStep — except on the wake, which lands in one
    // update. No wake-flash risk: the target is the same level the screen would have eased to.
    if (justWoke) {
      current_ = heldTarget_;
    } else if (current_ < heldTarget_) {
      current_ += cfg_.rampStep;
      if (current_ > heldTarget_) {
        current_ = heldTarget_;
      }
    } else if (current_ > heldTarget_) {
      current_ -= cfg_.rampStep;
      if (current_ < heldTarget_) {
        current_ = heldTarget_;
      }
    }
    if (current_ != lastSet_) {
      lastSet_ = current_;
      bl_.set(static_cast<uint8_t>(current_));
    }
  }

  IAmbientLight &ldr_;
  IBacklight &bl_;
  Config cfg_;

  bool enabled_ = true;
  bool awake_ = true;
  // Starts TRUE so the very first update() is not mistaken for a wake: boot deliberately keeps
  // its ramp up from black (current_ = 0), which is a power-on fade, not an answer to a touch.
  bool wasAwake_ = true;
  int32_t biasPct_ = 0;

  bool started_ = false;
  bool filterSeeded_ = false;
  uint32_t lastUpdateMs_ = 0;
  uint16_t lastRaw_ = 0;  // most recent raw LDR sample (diagnostics)
  float filtered_ = 0.0F; // low-passed LDR counts
  int32_t heldTarget_ = 0;
  int32_t current_ = 0; // starts at 0 so boot ramps up from black
  int32_t lastSet_ = -1;
};
