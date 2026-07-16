// LgfxDisplay — the IDisplay firmware adapter: lifecycle, geometry, brightness. Not pixels.
//
// Deliberately no blit/flush on the port. The LVGL flush pushes byte-swapped RGB565 by DMA inside
// a write session held open across a frame's chunks; a port carrying that contract would be
// LovyanGFX's shape with an I on the front — the move design.md §11 explicitly rejects for
// IHeaterSwitch ("push policy up, keep the port trivial"). Here the inverse applies: the flush
// holds no policy at all, only hardware sequencing, which is exactly what CLAUDE.md says
// src_cyd/main.cpp and the LGFX header exist to hold. There is also no second implementation to
// abstract over and no host test that could call it.
//
// Also implements IBacklight: IDisplay::setBrightness and IBacklight::set are the same operation,
// so this subsumes the old LgfxBacklight while AutoBrightness keeps depending on the narrow port
// it actually needs.
//
// The LGFX object is held by reference and must outlive this adapter.
#pragma once

#include "cyd_board.h"

#include "IBacklight.h"
#include "IDisplay.h"

class LgfxDisplay : public IDisplay, public IBacklight {
public:
  explicit LgfxDisplay(LGFX &gfx) : gfx_(gfx) {}

  bool begin() override { return gfx_.init(); }
  int width() const override { return gfx_.width(); }
  int height() const override { return gfx_.height(); }
  void setBrightness(uint8_t level) override { gfx_.setBrightness(level); }

  // IBacklight — the same knob under the name AutoBrightness asks for.
  void set(uint8_t level) override { setBrightness(level); }

private:
  LGFX &gfx_;
};
