// LgfxBacklight — the IBacklight firmware adapter (design.md §17, §18).
//
// Wraps the LGFX object's setBrightness(), which drives the GPIO21 LEDC PWM backlight
// (configured in include/LGFX_CYD2USB.hpp). Firmware-only glue in src_cyd/: it pulls in the
// LovyanGFX-backed LGFX type, so it never compiles for the native test targets (host tests
// drive AutoBrightness through FakeBacklight). The ramp/clamp/sleep behaviour lives in
// AutoBrightness (lib/app_logic); this stays a dumb setter.
//
// The LGFX object is held by reference and must outlive this adapter.
#pragma once

#include "LGFX_CYD2USB.hpp"

#include "IBacklight.h"

class LgfxBacklight : public IBacklight {
public:
  explicit LgfxBacklight(LGFX &gfx) : gfx_(gfx) {}
  void set(uint8_t level) override { gfx_.setBrightness(level); }

private:
  LGFX &gfx_;
};
