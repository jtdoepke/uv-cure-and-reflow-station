// Esp32AmbientLight — the IAmbientLight firmware adapter (design.md §18).
//
// A thin analogRead() wrapper over the CYD's on-board LDR. Firmware-only: it #includes
// <Arduino.h>, so — like the NVS/LGFX glue — it lives in src_cyd/ and never compiles for the
// native test targets (host tests drive AutoBrightness through FakeAmbientLight). The
// curve/filter that turn these raw counts into a backlight level live in AutoBrightness
// (lib/app_logic).
//
// The pin is a constructor argument rather than a constant on the class: which pin the LDR sits
// on is a fact about the board, so it belongs in cyd_board.h with every other one. setup() must
// also set that pin's ADC attenuation (kAmbientAtten) for the full ~0-3.3 V swing to be readable.
#pragma once

#include <Arduino.h>

#include "IAmbientLight.h"

class Esp32AmbientLight : public IAmbientLight {
public:
  explicit Esp32AmbientLight(int pin) : pin_(pin) {}
  uint16_t read() override { return static_cast<uint16_t>(analogRead(pin_)); }

private:
  int pin_;
};
