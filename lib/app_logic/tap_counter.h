// TapCounter — counts touch *presses* (rising edges) via the ITouch port.
//
// This is the seed "business logic" unit: pure C++, no Arduino, no LVGL, no LovyanGFX —
// it only knows the ITouch interface, so it runs under the native_logic test env with a
// FakeTouch injected. It demonstrates the ports-and-adapters pattern the oven controller
// will reuse (e.g. a start/stop button, a mode toggle), and the edge-detection here is the
// same debounce shape those controls will need: hold the panel and it still counts once.
//
// Usage: call poll() every loop iteration; count() returns how many distinct presses have
// happened. last_x()/last_y() expose the coordinates of the most recent press.
#pragma once

#include "ITouch.h"

class TapCounter {
public:
  explicit TapCounter(ITouch& touch) : touch_(touch) {}

  // Sample the touch panel once. Returns true if this call registered a *new* press
  // (a release->press transition), false otherwise.
  bool poll() {
    int x = 0, y = 0;
    bool pressed = touch_.getTouch(&x, &y);
    bool rising = pressed && !was_pressed_;
    if (rising) {
      ++count_;
      last_x_ = x;
      last_y_ = y;
    }
    was_pressed_ = pressed;
    return rising;
  }

  int count() const { return count_; }
  int last_x() const { return last_x_; }
  int last_y() const { return last_y_; }
  void reset() { count_ = 0; was_pressed_ = false; last_x_ = last_y_ = 0; }

private:
  ITouch& touch_;
  int count_ = 0;
  int last_x_ = 0;
  int last_y_ = 0;
  bool was_pressed_ = false;
};
