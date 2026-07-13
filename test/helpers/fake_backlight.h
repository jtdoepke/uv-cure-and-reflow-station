// Recording fake for IBacklight — injected into AutoBrightness under native_logic_cyd so the
// ramp/floor/ceiling behaviour can be asserted (last level pushed) and redundant writes checked.
// Header-only, shared via `#include "helpers/fake_backlight.h"`.
#pragma once

#include "IBacklight.h"

struct FakeBacklight : IBacklight {
  uint8_t level = 0; // last level pushed
  int setCalls = 0;  // total set() invocations (should skip redundant writes)
  void set(uint8_t l) override {
    level = l;
    setCalls++;
  }
};
