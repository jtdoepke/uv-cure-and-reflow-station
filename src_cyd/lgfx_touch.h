// LgfxTouch — the ITouch firmware adapter over LovyanGFX's getTouch().
//
// getTouch() returns coordinates already mapped to screen space by the panel's calibration (the
// cfg.x_min/x_max/y_min/y_max in the board's LGFX header), so this is a pure type adapter: no
// scaling, no rotation, no policy. Everything above it — wake-tap consumption, dev-tools
// injection — composes on the ITouch interface rather than reaching for the LGFX object.
//
// The LGFX object is held by reference and must outlive this adapter.
#pragma once

#include "cyd_board.h"

#include "ITouch.h"

class LgfxTouch : public ITouch {
public:
  explicit LgfxTouch(LGFX &gfx) : gfx_(gfx) {}

  bool getTouch(int *x, int *y) override {
    uint16_t tx = 0, ty = 0;
    if (!gfx_.getTouch(&tx, &ty)) {
      return false;
    }
    *x = tx;
    *y = ty;
    return true;
  }

private:
  LGFX &gfx_;
};
