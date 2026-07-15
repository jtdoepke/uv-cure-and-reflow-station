// TelemetrySender — the controller's hot path back to the CYD (design.md §9).
//
// The mirror of HeartbeatSender: fire-on-tick at kTelemetryPeriodMs, no retransmit, no ACK. A
// lost frame self-heals on the next one, so nothing here is reliable-delivery shaped.
//
// Sent **unconditionally, run or no run** (§9: on boot the controller "unconditionally sends
// Hello + IDLE telemetry"). That matters beyond the live temp graph: this stream is the CYD's
// *only* evidence the controller exists at all. The CYD's heartbeat proves the CYD is alive to
// the controller; nothing proves the reverse, so without this the CYD cannot tell a healthy
// controller from an unplugged one and would keep claiming "Link" over a dead cable.
//
// The caller owns the payload and mutates it in place via state(): this class only stamps
// session/seq/ctrl_millis and owns the cadence. That keeps it ignorant of what telemetry
// *means* — A5/A6 fill in temps, setpoint, and duty as those land; today the controller emits
// an otherwise-zeroed IDLE frame, which is exactly what §9 asks for.
//
// Reads the clock internally in service(), same idiom as HeartbeatSender. Keep free of
// <Arduino.h>.
#pragma once

#include <cstdint>

#include "IClock.h"
#include "frame_link.h"
#include "messages.h"

namespace protocol {

class TelemetrySender {
public:
  // link and clock must outlive this sender.
  TelemetrySender(FrameLink &link, IClock &clock) : link_(link), clock_(clock) {}

  // The frame to send, for the caller to populate. session/seq/ctrl_millis are overwritten on
  // every send, so callers should not bother setting them.
  oven_Telemetry &state() { return state_; }
  const oven_Telemetry &state() const { return state_; }

  // The session this telemetry reports. 0 = no active session, i.e. IDLE telemetry, which is
  // what a freshly booted (or freshly safed) controller sends.
  void setSession(uint32_t session) { session_ = session; }
  uint32_t session() const { return session_; }

  // Emit on the period boundary. Call every loop iteration.
  void service();

  // Emit immediately and restart the period.
  void sendNow();

  uint32_t seq() const { return seq_; }

private:
  FrameLink &link_;
  IClock &clock_;

  oven_Telemetry state_ = oven_Telemetry_init_default;
  uint32_t session_ = 0;
  uint32_t seq_ = 0;
  uint32_t last_send_ms_ = 0;
  bool sent_ = false;
};

} // namespace protocol
