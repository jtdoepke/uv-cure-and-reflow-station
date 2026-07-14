// SafetySupervisor — the controller's output safety gate (design.md §4, backlog A4a).
//
// The single owner of the heater's safety cutoff and the mains contactor. Every
// controller loop it re-derives, from the reliability layer, whether the oven may
// run *right now* and drives the outputs to match. It only ever forces OFF / opens;
// it never enables — the PID (A5) owns the heater duty, and this class has the last
// word over it (call tick() last in the loop).
//
// Run authorization is the composed L1 term from §9 (ControllerLink::authorized()):
// handshake matched && active session && heartbeat fresh within the 750 ms command-
// timeout && the last heartbeat's enable bit. When that drops for any reason — a
// pulled CYD TX, a rebooted CYD, enable cleared, schema skew — the heater is forced
// OFF and the contactor opened on the very next tick. Fail-safe by construction:
// silence and staleness read as "stop", never as consent.
//
// Contactor policy (§4): energize-to-close, energized *only while a run is actively
// commanded*, i.e. only while authorized() && !faulted(). Idle, faulted, a lost link
// past the timeout, or enable=false all de-energize the coil, removing mains — the
// stricter reading of "mains present only while actively, freshly commanded". (A4b
// may refine this to let the contactor ride through brief SSR-off periods.)
//
// A4a scope: L1 command-timeout + fail-safe defaults + contactor policy. The trip()
// latch is the seam A4b's L3 clamps (setpoint/over-temp/stuck-heater/runtime) and
// the A6/A7 fault paths hook into; here it only needs to exist and be honored — no
// FaultCode plumbing or Fault emission yet.
//
// Lives in control_logic (composes ControllerLink + HeaterActuator + IContactor);
// all three are injected by reference and must outlive this object. Header-only.
#pragma once

#include "IContactor.h"
#include "controller_link.h"
#include "heater_actuator.h"

class SafetySupervisor {
public:
  SafetySupervisor(ControllerLink &link, HeaterActuator &heater, IContactor &contactor)
      : link_(link), heater_(heater), contactor_(contactor) {
    // Fail-safe default before the first tick(): heater OFF, mains isolated.
    heater_.forceOff();
    contactor_.setClosed(false);
  }

  // Call every controller loop, last (after the PID sets duty and the actuator
  // ticks), so safety overrides whatever was commanded.
  void tick() {
    const bool run = link_.authorized() && !faulted_;
    setContactor(run);
    if (!run) {
      heater_.forceOff();
    }
    // run==true: leave the heater duty to the control loop; the supervisor only cuts.
  }

  // Latch the oven into the safe state (contactor open, heater off) until
  // clearFault(). The seam A4b's L3 clamps and the A6/A7 fault paths drive.
  void trip() {
    faulted_ = true;
    heater_.forceOff();
    setContactor(false);
  }

  // Clear the latch. Outputs stay safe until the next tick() re-derives run.
  void clearFault() { faulted_ = false; }

  bool faulted() const { return faulted_; }
  // The oven is safe iff mains is isolated (the contactor is the last electrical line).
  bool safe() const { return !contactor_closed_; }

private:
  // Drive the coil only on an actual state change (avoid redundant coil writes).
  void setContactor(bool closed) {
    if (closed != contactor_closed_) {
      contactor_closed_ = closed;
      contactor_.setClosed(closed);
    }
  }

  ControllerLink &link_;
  HeaterActuator &heater_;
  IContactor &contactor_;
  bool faulted_ = false;
  bool contactor_closed_ = false; // mirrors the constructor's fail-safe open
};
