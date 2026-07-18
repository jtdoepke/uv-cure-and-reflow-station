// settings_codec.h — convert the CYD's domain Settings (settings_store.h) to/from the wire
// oven_Settings (§9). Added with Wave R3b of the §2 "CYD is a UI remote" split: device settings
// persist on the controller now, so the CYD keeps an in-RAM SettingsStore synced over the link —
// fetched on connect, pushed on change — and this codec bridges the two representations.
//
// Pure C++ over nanopb + settings_store.h — no LVGL, no Arduino.
#pragma once

#include "oven.pb.h"
#include "settings_store.h"

namespace settings_codec {

inline oven_TempUnits unitsToWire(TempUnits u) {
  return u == TempUnits::Fahrenheit ? oven_TempUnits_TEMP_UNITS_FAHRENHEIT
                                    : oven_TempUnits_TEMP_UNITS_CELSIUS;
}
inline TempUnits unitsFromWire(oven_TempUnits u) {
  return u == oven_TempUnits_TEMP_UNITS_FAHRENHEIT ? TempUnits::Fahrenheit : TempUnits::Celsius;
}

inline oven_Settings toWire(const Settings &s) {
  oven_Settings w = oven_Settings_init_zero;
  w.units = unitsToWire(s.units);
  w.auto_brightness = s.autoBrightness;
  w.advanced_unlocked = s.advancedUnlocked;
  w.brightness_bias = s.brightnessBias;
  w.screen_brightness_pct = s.screenBrightnessPct;
  w.idle_timeout_min = s.idleTimeoutMin;
  w.uv_max_cap = s.uvMaxCap;
  w.reflow_max_cap = s.reflowMaxCap;
  return w;
}

inline Settings fromWire(const oven_Settings &w) {
  Settings s;
  s.units = unitsFromWire(w.units);
  s.autoBrightness = w.auto_brightness;
  s.advancedUnlocked = w.advanced_unlocked;
  s.brightnessBias = w.brightness_bias;
  s.screenBrightnessPct = w.screen_brightness_pct;
  s.idleTimeoutMin = w.idle_timeout_min;
  s.uvMaxCap = w.uv_max_cap;
  s.reflowMaxCap = w.reflow_max_cap;
  return s;
}

} // namespace settings_codec
