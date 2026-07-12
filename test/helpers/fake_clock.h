// Fake time source for the controller ports — injected into logic under the
// native_control env so timeout behavior is tested deterministically, no
// sleeps. Header-only, shared via `#include "helpers/fake_clock.h"`.
#pragma once

#include "IClock.h"

// Programmable clock: advance() it by hand between calls to the code under test.
struct FakeClock : IClock {
  uint32_t now = 0;
  uint32_t millis() override { return now; }
  void advance(uint32_t ms) { now += ms; }
};
