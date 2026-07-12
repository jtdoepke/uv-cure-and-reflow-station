// HeartbeatSender — the CYD's fire-on-tick liveness + run authorization
// (design.md §9). Emits Heartbeat{session, seq, enable, millis} every
// kHeartbeatPeriodMs; a lost tick self-heals on the next (no retransmit). The
// controller's SessionGate treats silence past the command-timeout, or
// enable=false, as "de-authorize" — so simply calling service() on a steady
// cadence is what keeps a run alive, and stopping it (or setEnable(false)) safes
// the oven.
//
// Reads the clock internally in service() (same idiom as HeartbeatMonitor).
// Keep free of <Arduino.h>.
#pragma once

#include <cstdint>

#include "IClock.h"
#include "frame_link.h"

namespace protocol {

class HeartbeatSender {
public:
  // link and clock must outlive this sender. session/enable start at 0/false;
  // the CYD sets them as a run begins (setSession from the Start it sent).
  HeartbeatSender(FrameLink &link, IClock &clock) : link_(link), clock_(clock) {}

  // The run this heartbeat authorizes (0 = none). A rebooted CYD presents a new
  // session, so the controller times out rather than honoring a stale one.
  void setSession(uint32_t session) { session_ = session; }
  uint32_t session() const { return session_; }

  // The explicit HEAT_EN bit. enable=false de-authorizes immediately (no latch).
  void setEnable(bool enable) { enable_ = enable; }
  bool enable() const { return enable_; }

  // Emit a heartbeat if kHeartbeatPeriodMs has elapsed since the last one. Call
  // every loop iteration.
  void service();

  // Emit one immediately regardless of cadence (e.g. to authorize right after a
  // Start is acked). Advances seq and resets the period timer.
  void sendNow();

private:
  FrameLink &link_;
  IClock &clock_;
  uint32_t session_ = 0;
  uint32_t seq_ = 0;
  bool enable_ = false;
  uint32_t last_send_ms_ = 0;
  bool sent_ = false;
};

} // namespace protocol
