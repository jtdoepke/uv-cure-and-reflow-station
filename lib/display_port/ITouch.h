// ITouch — narrow, Arduino-free port for the touch panel.
//
// getTouch() mirrors LovyanGFX's LGFX::getTouch(): it returns true while the panel is
// being pressed and writes already-calibrated *screen* coordinates to x/y (LovyanGFX does
// the XPT2046 raw->screen mapping for us). Logic that reacts to touch depends on this
// interface, so host tests inject a FakeTouch and never link LovyanGFX.
#pragma once

struct ITouch {
  virtual ~ITouch() = default;
  virtual bool getTouch(int* x, int* y) = 0;
};
