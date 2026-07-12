// IClock — fakeable time source for the oven controller (design.md §11).
//
// Every timeout-shaped decision (heartbeat staleness, heater time-proportioning
// windows, per-segment watchdogs) reads time through this port so it can be
// unit-tested on the host with a FakeClock advanced by hand. The production
// adapter wraps Arduino millis().
//
// Keep this header free of <Arduino.h> so it stays native-compilable.
#pragma once

#include <cstdint>

struct IClock {
  virtual ~IClock() = default;
  virtual uint32_t millis() = 0; // monotonic ms since boot; wraps at 2^32 like Arduino's
};
