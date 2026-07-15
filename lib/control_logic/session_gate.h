// SessionGate — the controller's run-authorization gate (design.md §9).
//
// Combines the four terms that decide whether the heater/UV may run *right now*
// into one boolean the SafetySupervisor (A4) polls:
//
//   authorized() = handshake matched   (schema-hash gate, no version skew)
//                && a session is active (adopted from an accepted Start)
//                && heartbeat is fresh  (within the 750 ms command-timeout)
//                && the last heartbeat's enable bit is set (explicit HEAT_EN).
//
// No latch: every accepted heartbeat overwrites the enable bit and freshness is
// time-based, so enable=false or silence de-authorizes on the next authorized()
// read. Heartbeats for any other session are ignored — a rebooted/stale CYD that
// has forgotten its session authorizes nothing, so the controller stays IDLE and
// safe. This is the reliability seam only; the run state machine (A6) and the
// output-driving safety logic (A4) live above it.
//
// Lives in control_logic (it owns the session policy); reuses protocol::HeartbeatMonitor for
// freshness rather than re-deriving wraparound-safe timing.
#pragma once

#include <cstdint>

#include "IClock.h"
#include "handshake.h"
#include "heartbeat_monitor.h"
#include "link_params.h"
#include "oven.pb.h"

class SessionGate {
public:
  // clock and handshake must outlive this gate.
  SessionGate(IClock &clock, protocol::Handshake &handshake)
      : monitor_(clock), handshake_(handshake) {}

  // Adopt the run this session authorizes (from an accepted Start). Clears the
  // freshness monitor so authorization waits for the first heartbeat of the new
  // session rather than inheriting the previous run's liveness.
  void adoptSession(uint32_t session) {
    active_session_ = session;
    has_active_ = true;
    last_enable_ = false;
    monitor_.reset();
  }

  // Drop the active session (run ended/aborted/faulted). Returns to IDLE-safe.
  void clearSession() {
    has_active_ = false;
    last_enable_ = false;
    monitor_.reset();
  }

  // Feed a decoded heartbeat. Session filter: a heartbeat for any session other
  // than the active one is ignored entirely (not fed), so it authorizes nothing.
  void onHeartbeat(const oven_Heartbeat &hb) {
    if (!has_active_ || hb.session != active_session_) {
      return;
    }
    monitor_.feed();
    last_enable_ = hb.enable;
  }

  // The composed authorization the SafetySupervisor consumes.
  bool authorized() const {
    return handshake_.matched() && has_active_ && !monitor_.expired(protocol::kCommandTimeoutMs) &&
           last_enable_;
  }

  bool hasActiveSession() const { return has_active_; }
  uint32_t activeSession() const { return active_session_; }

private:
  protocol::HeartbeatMonitor monitor_;
  protocol::Handshake &handshake_;
  uint32_t active_session_ = 0;
  bool has_active_ = false;
  bool last_enable_ = false;
};
