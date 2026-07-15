// HeartbeatMonitor — "has the peer's periodic frame arrived recently?" (design.md §9).
//
// Both ends of the link ask that same question of the other's hot-path stream, which is why
// this lives in lib/protocol rather than on either side: SessionGate (controller) asks it of the
// CYD's Heartbeat and safes at kCommandTimeoutMs; CydLink (CYD) asks it of the controller's
// Telemetry and reports the link down at kLinkTimeoutMs. One implementation, both directions.
//
// A monitor that has never been fed is expired — silence must never read as consent. That
// invariant is the whole reason this is a class rather than an inline subtraction.
//
// Pure C++ behind the IClock port, tested in native_control with a FakeClock. (CYD app-layer
// logic that owns no port — FaultController, SleepController — deliberately reuses the *pattern*
// with a passed-in `now` instead, rather than drag lib/control_port into that lane for ~12
// lines. This class is for code that already holds a clock.)
#pragma once

#include <cstdint>

#include "IClock.h"

namespace protocol {

class HeartbeatMonitor {
public:
  explicit HeartbeatMonitor(IClock &clock) : clock_(clock) {}

  // Record that a valid frame arrived now.
  void feed() {
    last_fed_ms_ = clock_.millis();
    fed_ = true;
  }

  // True when nothing has arrived within the last window_ms (or ever).
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

} // namespace protocol
