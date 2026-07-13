// IAmbientLight — bare ambient-light readback for auto-brightness (design.md §18).
//
// Deliberately dumb: it returns the raw ADC counts from the CYD's on-board LDR (GPIO34,
// ADC1 — reads fine with WiFi on). All the interesting work — low-pass filtering, the
// perceptual lux->brightness curve, hysteresis and clamping — is portable logic in
// lib/app_logic (AutoBrightness), NOT the port's job, so it stays host-testable against a
// FakeAmbientLight. The production adapter is a thin analogRead() wrapper in src_cyd/.
//
// Keep this header free of <Arduino.h> so it stays native-compilable.
#pragma once

#include <cstdint>

struct IAmbientLight {
  virtual ~IAmbientLight() = default;
  virtual uint16_t read() = 0; // raw ADC counts (0..4095 on the ESP32's 12-bit ADC1)
};
