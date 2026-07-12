// HeartbeatMonitor — tracks freshness of the CYD's heartbeat (design.md §9).
//
// This is the controller-side seed logic unit (the role TapCounter plays for
// the CYD): pure C++ behind the IClock port, tested in the native_control env
// with a FakeClock. The reliability layer (backlog A2) will feed() it on every
// valid Heartbeat frame; the SafetySupervisor (A4) will treat expired() as
// "link lost -> outputs off". A monitor that has never been fed is expired:
// silence must never read as consent.
#pragma once

#include <cstdint>

#include "IClock.h"

class HeartbeatMonitor {
public:
  explicit HeartbeatMonitor(IClock &clock) : clock_(clock) {}

  // Record that a valid heartbeat arrived now.
  void feed() {
    last_fed_ms_ = clock_.millis();
    fed_ = true;
  }

  // True when no heartbeat has arrived within the last window_ms (or ever).
  // Unsigned subtraction keeps this correct across millis() wraparound.
  bool expired(uint32_t window_ms) const {
    if (!fed_) {
      return true;
    }
    return static_cast<uint32_t>(clock_.millis() - last_fed_ms_) >= window_ms;
  }

  void reset() { fed_ = false; }

private:
  IClock &clock_;
  uint32_t last_fed_ms_ = 0;
  bool fed_ = false;
};
