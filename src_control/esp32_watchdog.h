// Esp32Watchdog — the IWatchdog firmware adapter (design.md §9, §11).
//
// The ESP32's Task Watchdog, watching the task that calls kick() — the Arduino loop task.
// Deliberately NOT Arduino's enableLoopWDT(): that makes the loop task's wrapper feed the dog
// on every iteration, which proves the wrapper is alive rather than that our loop() actually
// reached its kick(). Subscribing the task ourselves and feeding only from kick() keeps the
// thing being proven the same as the thing being asked (§11).
//
// A timeout panics and resets; the outputs' hardware pull-downs then take heater and contactor
// to safe with no firmware involved, and the next boot reads the cause back out of
// esp_reset_reason(). The cause is cached at begin() because it must be read before anything
// else has a chance to clear it.
//
// Firmware-only (<Arduino.h> + esp_task_wdt.h), so host tests use FakeWatchdog instead.
#pragma once

#include <Arduino.h>
#include <esp_system.h>
#include <esp_task_wdt.h>

#include "IWatchdog.h"

class Esp32Watchdog : public IWatchdog {
public:
  // Comfortably longer than a worst-case controller loop (LVGL lives on the other MCU; ours is
  // link + control only), short enough that a hang is caught long before it could matter
  // thermally. A4b may tighten this once the real loop's timing is measured.
  static constexpr uint32_t kTimeoutMs = 5000;

  // Call from setup(), on the same task that will call kick().
  void begin(uint32_t timeout_ms = kTimeoutMs) {
    raw_ = esp_reset_reason();
    cause_ = classify(raw_);

    esp_task_wdt_config_t cfg = {};
    cfg.timeout_ms = timeout_ms;
    cfg.idle_core_mask = 0;   // we watch our own loop, not the idle tasks
    cfg.trigger_panic = true; // panic -> reset -> pull-downs -> safe (§11)
    // Arduino-ESP32 may have initialized the TWDT already; adopt our config either way.
    if (esp_task_wdt_init(&cfg) == ESP_ERR_INVALID_STATE) {
      esp_task_wdt_reconfigure(&cfg);
    }
    esp_task_wdt_add(nullptr); // subscribe the calling (loop) task
  }

  void kick() override { esp_task_wdt_reset(); }

  ResetCause lastResetCause() const override { return cause_; }

  // Adapter-only, not part of the port: the unmapped reason, for the bench banner — so a
  // surprising classification can be diagnosed rather than guessed at.
  esp_reset_reason_t rawResetReason() const { return raw_; }

private:
  static ResetCause classify(esp_reset_reason_t r) {
    switch (r) {
    case ESP_RST_POWERON:
    case ESP_RST_EXT: // the EN pin / reset button — an ordinary start
      return ResetCause::PowerOn;
    case ESP_RST_TASK_WDT:
    case ESP_RST_INT_WDT:
    case ESP_RST_WDT:
      return ResetCause::Watchdog;
    case ESP_RST_PANIC:
      return ResetCause::Panic;
    case ESP_RST_SW:
      return ResetCause::Software;
    case ESP_RST_BROWNOUT:
      return ResetCause::Brownout;
    default:
      return ResetCause::Other;
    }
  }

  esp_reset_reason_t raw_ = ESP_RST_UNKNOWN;
  ResetCause cause_ = ResetCause::Other;
};
