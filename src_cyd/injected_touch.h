// InjectedTouch — an ITouch that lets the WiFi dev API pre-empt the panel (UI_DEV_TOOLS only).
//
// A decorator rather than an #if inside the indev callback: an injected touch IS a source of
// touches, which is precisely what ITouch names. Composing it means the callback contains one
// touch read and no build-flag branch, and the "injection wins over the panel" rule lives in one
// place instead of being inlined into the hot path.
//
// This is what earns ITouch its keep on this board. Not host-testability — nothing new gets
// tested — but that the capability composes at the port instead of being an #if in main.cpp.
#pragma once

#include "ITouch.h"
#include "ui_dev_tools.h"

class InjectedTouch : public ITouch {
public:
  explicit InjectedTouch(ITouch &panel) : panel_(panel) {}

  bool getTouch(int *x, int *y) override {
    int16_t sx = 0, sy = 0;
    if (ui_dev_touch_get(&sx, &sy)) { // an injected touch pre-empts the panel
      *x = sx;
      *y = sy;
      return true;
    }
    return panel_.getTouch(x, y);
  }

private:
  ITouch &panel_;
};
