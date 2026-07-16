// SleepController — the idle-sleep/wake policy for the CYD display (design.md §17).
//
// "Sleep" here is backlight-off only (the MCU + UART link stay alive) — this class just decides
// awake vs asleep; AutoBrightness (§18) owns the actual backlight and ramps it off when told
// setAwake(false). Pure, host-testable logic: it takes time as a parameter and a caller-computed
// `sleepAllowed` predicate, so it never touches LVGL/Arduino and stays in the fast
// native_logic_cyd lane.
//
// Policy (§17):
//   - Sleep only when idle AND cool. The caller passes sleepAllowed = (run_state == RUN_IDLE);
//     the run-state subject already collapses HOT/running/fault into non-idle states, so
//     "never sleep during a run" and "stay awake while HOT" fall out of a single predicate.
//   - While !sleepAllowed the inactivity timer is held reset, so returning to idle grants a
//     full fresh timeout before sleeping.
//   - Any activity (a touch wake-tap, a fault, a door-open event) calls noteActivity() to wake
//     and restart the timer.
#pragma once

#include <cstdint>

class SleepController {
public:
  struct Config {
    uint32_t idleTimeoutMs = 120000; // ~2 min default (§17); configurable in Settings (§24)
  };

  explicit SleepController(Config cfg) : idleTimeoutMs_(cfg.idleTimeoutMs) {}
  SleepController() : SleepController(Config{}) {}

  // Wake and restart the inactivity timer. Called on any touch, and on a fault/door-open wake
  // event (§17/§22 — never hide a fault behind a dark screen).
  void noteActivity(uint32_t nowMs) {
    lastActivityMs_ = nowMs;
    awake_ = true;
  }

  // Pushed from settings each loop (idleTimeoutMin() * 60000).
  void setIdleTimeoutMs(uint32_t ms) { idleTimeoutMs_ = ms; }

  // Call every loop. `sleepAllowed` is the caller's idle-AND-cool predicate. Sleeps once the
  // display has been idle (no activity, sleep permitted) for the timeout.
  void tick(uint32_t nowMs, bool sleepAllowed) {
    if (!started_) {
      started_ = true;
      lastActivityMs_ = nowMs;
      return;
    }
    if (!sleepAllowed) {
      awake_ = true;
      lastActivityMs_ = nowMs; // hold the timer reset while a run/HOT/fault keeps us awake
      return;
    }
    // Signed elapsed, deliberately. Two things must both hold:
    //   - millis() wraps every ~49 days and this machine can sit powered for months, so the
    //     subtraction must stay modular (never compare timestamps directly).
    //   - noteActivity() can legitimately carry a timestamp LATER than this nowMs: main.cpp
    //     samples `now` at the top of its loop, then lv_timer_handler() runs the touch callback,
    //     which stamps the wake with a fresh millis(). Unsigned, that few-ms skew wrapped to
    //     ~4.29e9 and instantly satisfied any timeout — waking, then sleeping again on the very
    //     next tick. Signed, the same subtraction simply reads as a small negative and no
    //     timeout has elapsed, which is the truth.
    // int32_t is wide enough: idleTimeoutMs_ is minutes (§24 caps it at 10), not weeks.
    const int32_t elapsed = static_cast<int32_t>(nowMs - lastActivityMs_);
    if (awake_ && elapsed >= static_cast<int32_t>(idleTimeoutMs_)) {
      awake_ = false;
    }
  }

  bool awake() const { return awake_; }

private:
  uint32_t idleTimeoutMs_;
  bool awake_ = true;
  bool started_ = false;
  uint32_t lastActivityMs_ = 0;
};
