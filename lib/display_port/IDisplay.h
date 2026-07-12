// IDisplay — narrow, Arduino-free port for the display.
//
// This is the hardware boundary: business logic depends on this interface, never on
// LovyanGFX directly, so it can be compiled and unit-tested on the host (see the
// native_logic_cyd test env). The production implementation is a thin adapter that wraps the
// LGFX object (`gfx.init()`, `gfx.width()`, ...); tests inject a FakeDisplay instead.
//
// Keep this header free of <Arduino.h> and <LovyanGFX.hpp> so it stays native-compilable.
#pragma once

#include <cstdint>

struct IDisplay {
  virtual ~IDisplay() = default;
  virtual bool begin() = 0; // mirrors LGFX::init() -> bool
  virtual int width() const = 0;
  virtual int height() const = 0;
  virtual void setBrightness(uint8_t level) = 0;
};
