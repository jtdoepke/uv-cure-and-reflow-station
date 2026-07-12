// Test doubles for the display/touch ports — injected into logic under the native_logic_cyd
// env so tests never link LovyanGFX. Header-only (no .cpp to compile); shared across
// native suites via `#include "helpers/fake_touch.h"` (test/ root is on the include path).
#pragma once

#include "IDisplay.h"
#include "ITouch.h"

// Programmable touch: set `nextTouch` (and tx/ty) before calling the code under test.
struct FakeTouch : ITouch {
  bool nextTouch = false;
  int tx = 0;
  int ty = 0;
  bool getTouch(int *x, int *y) override {
    if (nextTouch) {
      *x = tx;
      *y = ty;
    }
    return nextTouch;
  }
};

// Inspectable display: records the last brightness; reports fixed dimensions.
struct FakeDisplay : IDisplay {
  bool up = true;
  int w = 320;
  int h = 240;
  uint8_t bright = 0;
  bool begin() override { return up; }
  int width() const override { return w; }
  int height() const override { return h; }
  void setBrightness(uint8_t level) override { bright = level; }
};
