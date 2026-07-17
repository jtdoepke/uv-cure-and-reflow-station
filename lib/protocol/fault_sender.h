// FaultSender — the controller's Fault annunciation to the CYD (design.md §9/§22, backlog A4b).
//
// The dedicated Fault{session, code} frame the CYD's FaultController consumes to raise the §22
// modal. Unlike the telemetry stream (which also carries fault_code as a continuous backup),
// this frame is event-shaped: it fires the moment SafetySupervisor::faultCode() changes to a
// real code, then re-sends every kFaultResendMs while that code stays active so a dropped,
// un-ACKed frame self-heals. The CYD latches on first receipt, so the repeats are harmless.
//
// The caller drives it from the supervisor's active code each loop: set() the current code and
// service() on the cadence. FAULT_NONE means "no fault" — nothing is emitted for it, and a
// transition back to NONE simply stops the re-sends (the CYD's own ack/condition-clear owns
// dismissal, §22). Because the caller passes the code every loop, this class holds no policy
// about what a fault means; it owns only the frame and the cadence, mirroring TelemetrySender.
//
// Reads the clock internally in service() (same idiom as HeartbeatSender/TelemetrySender).
// Keep free of <Arduino.h>.
#pragma once

#include <cstdint>

#include "IClock.h"
#include "frame_link.h"
#include "oven.pb.h"

namespace protocol {

class FaultSender {
public:
  // link and clock must outlive this sender.
  FaultSender(FrameLink &link, IClock &clock) : link_(link), clock_(clock) {}

  // The session the fault belongs to (0 = none / idle). Stamped on every emitted frame.
  void setSession(uint32_t session) { session_ = session; }
  uint32_t session() const { return session_; }

  // The controller's currently active fault code (typically SafetySupervisor::faultCode()).
  // A change to a new non-NONE code emits immediately; a change to FAULT_NONE stops re-sending.
  // Idempotent: passing the same code twice does not re-emit outside the resend cadence.
  void set(oven_FaultCode code);

  // Emit again if a fault is active and kFaultResendMs has elapsed. Call every loop iteration.
  void service();

  oven_FaultCode active() const { return code_; }

private:
  void sendNow();

  FrameLink &link_;
  IClock &clock_;
  uint32_t session_ = 0;
  oven_FaultCode code_ = oven_FaultCode_FAULT_NONE;
  uint32_t last_send_ms_ = 0;
};

} // namespace protocol
