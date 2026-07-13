// Esp32AmbientLight — the IAmbientLight firmware adapter (design.md §18).
//
// A thin analogRead() wrapper over the CYD's on-board LDR on GPIO34 (ADC1, which reads fine
// with WiFi on). Firmware-only: it #includes <Arduino.h>, so — like the NVS/LGFX glue — it
// lives in src_cyd/ and never compiles for the native test targets (host tests drive
// AutoBrightness through FakeAmbientLight). The curve/filter that turn these raw counts into a
// backlight level live in AutoBrightness (lib/app_logic).
//
// setup() should set the pin's ADC attenuation (analogSetPinAttenuation(kPin, ADC_11db)) so the
// full ~0-3.3 V LDR swing is readable.
#pragma once

#include <Arduino.h>

#include "IAmbientLight.h"

class Esp32AmbientLight : public IAmbientLight {
public:
  static constexpr int kPin = 34; // on-board LDR, ADC1 (design.md §18 pin inventory)
  uint16_t read() override { return static_cast<uint16_t>(analogRead(kPin)); }
};
