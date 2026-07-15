// IBacklight — bare brightness gate for the display backlight (design.md §17, §18).
//
// Deliberately just a setter: the smooth ramping, ambient curve and sleep/wake gating are
// portable logic in lib/app_logic (AutoBrightness), NOT the port's job, so they stay
// host-testable against a recording fake. The production adapter drives the GPIO21 LEDC PWM
// via LovyanGFX setBrightness() (see the board LGFX header selected by include/cyd_board.h) and
// lives in src_cyd/.
//
// A separate, narrower port from IDisplay: AutoBrightness only needs to set a level, so it
// depends on this one method rather than the whole display surface.
//
// Keep this header free of <Arduino.h> so it stays native-compilable.
#pragma once

#include <cstdint>

struct IBacklight {
  virtual ~IBacklight() = default;
  virtual void set(uint8_t level) = 0; // 0 = off, 255 = full
};
