// fault_controller.h — the CYD's fault latch + ack routing (design.md §22, backlog B7).
//
// §22's architecture note: "Latching + ack-routing logic is host-testable state, not view code."
// This is that state. C8's FaultViewModel binds `lv_subject_t`s to state() and calls
// acknowledge(); the overlay, the RGB-LED/buzzer annunciation (pattern TBD §10), and the
// lv_layer_top object are all C8's — this class holds no `lv_` types so the gateway can marshal
// it onto the UI task.
//
// The whole shape is a two-state latch, because that is what §22 specifies — not a workflow:
//   Clear --(controller Fault | link-loss edge during a run)--> Latched --(acknowledge)--> Clear
// "Never auto-dismiss ... even if the condition clears" is the headline rule: nothing but an
// explicit acknowledge() leaves Latched. A fault is a human-in-the-loop event.
//
// Two trigger origins (§22): a controller `Fault{session, code}` frame, and a CYD-self-raised
// LINK_LOST when our own heartbeat times out *during a run* (mid-run the UART can go silent, so
// no Fault can arrive; the safety invariant still holds — the controller safes on its own
// command-timeout — and the overlay says exactly that rather than pretending to confirm state).
//
// No clock, no port: tick(nowMs, linkHealthy) mirrors SleepController::tick(nowMs, sleepAllowed)
// — time and the health predicate are passed in, so this stays in the fast native_logic_cyd lane.
// It deliberately does NOT reuse lib/control_logic/heartbeat_monitor.h: that is controller-tree
// code bound to the IClock port, and dragging lib/control_port into the CYD lane to reuse ~12
// lines would be a worse trade than reusing the *pattern*.
#pragma once

#include <cstdint>

#include "fault_table.h"
#include "oven.pb.h"

// Where Acknowledge sends the operator (§22). C8's screen manager maps these to screens.
enum class AckRoute : uint8_t {
  None,       // nothing was latched — acknowledge() was a no-op
  Home,       // no run was active when the fault raised
  RunSummary, // a run was active → the Fault-outcome summary (§16)
};

// An immutable snapshot for C8's gateway to diff into lv_subject_t (§22's MVVM note). POD, no lv_.
struct FaultState {
  bool active;         // latched and unacknowledged
  oven_FaultCode code; // the current (highest-severity-so-far) cause
  uint32_t session;    // session of the frame that set `code`; for Details + the log
  uint32_t count;      // faults latched this episode; the view shows "+N" for count-1
  fault_table::Severity severity;
  bool overTemp;        // sticky OR over the episode → HOT persists (§14/§17)
  uint32_t raisedAtMs;  // when this episode began
  uint32_t updatedAtMs; // bumps on every raise/update → cheap change detection for C8
};

class FaultController {
public:
  struct Config {
    // The CYD's own heartbeat timeout that self-raises LINK_LOST during a run (§22 origin 2).
    // Unused while the caller supplies `linkHealthy` to tick() — kept so this class can own the
    // timeout if no CYD-side link owner (C7/B6 era) materialises. §10 is silent on this value;
    // §9 only specifies the controller-side 750 ms command-timeout. PLACEHOLDER.
    uint32_t linkTimeoutMs = 2000; // TBD §10
  };

  explicit FaultController(Config cfg) : cfg_(cfg) {}
  FaultController() : FaultController(Config{}) {}

  // Pushed from the telemetry gateway. Sticky at raise time (see runActiveAtRaise_): a
  // controller that flips RUNNING→FAULT in the same breath as the Fault frame must not be able
  // to turn a RunSummary route into a Home route and lose the aborted run its record.
  void setRunActive(bool runActive) { runActive_ = runActive; }

  // §22 origin 1: a controller Fault{session, code} frame arrived.
  //
  // Records `session` but filters nothing — stale-session filtering belongs to the gateway,
  // mirroring the controller's own session_gate.h. Every call counts toward `+N`, including a
  // repeat of the same code: A2's seq/dedup should prevent a storm, and if one happens the
  // count is a real signal that the gateway is misbehaving rather than something to hide here.
  void onControllerFault(uint32_t nowMs, uint32_t session, oven_FaultCode code) {
    if (code == oven_FaultCode_FAULT_NONE) {
      return; // the enum's zero value is "no fault"; a frame carrying it is malformed.
    }
    raise(nowMs, session, code);
  }

  // Call every loop with the caller's link-health predicate (the SleepController idiom).
  //
  // Edge-triggered: tick() runs at loop rate, so a level-triggered self-raise would increment
  // `count` thousands of times per second. Only the healthy→unhealthy transition raises, and
  // only while a run is active — §22 scopes this to "CYD-detected link loss *during a run*"; an
  // idle link loss is the §13 header indicator, not this modal.
  //
  // A recovering link re-arms the self-raise but never dismisses: §22's latching rule.
  void tick(uint32_t nowMs, bool linkHealthy) {
    if (linkHealthy) {
      linkLostRaised_ = false;
      return;
    }
    if (!runActive_ || linkLostRaised_) {
      return;
    }
    linkLostRaised_ = true;
    // Session 0: we never received a frame to attribute this to — we inferred it from silence.
    raise(nowMs, 0, oven_FaultCode_FAULT_LINK_LOST);
  }

  FaultState state() const {
    return FaultState{active_,   code_,     session_,    count_,
                      severity_, overTemp_, raisedAtMs_, updatedAtMs_};
  }
  bool active() const { return active_; }

  // §22: "Acknowledge is a plain single tap ... always allowed — it dismisses the *alarm*, not
  // the *hazard*" — deliberately not gated on the condition clearing. overTempLatched() survives
  // so the caller's HOT/sleep policy carries the residual state forward.
  AckRoute acknowledge() {
    if (!active_) {
      return AckRoute::None;
    }
    active_ = false;
    return runActiveAtRaise_ ? AckRoute::RunSummary : AckRoute::Home;
  }

  // Sticky past acknowledge() (§22: "the HOT state (§14) persists on Home and keeps suppressing
  // sleep (§17) until the chamber cools"). The caller ORs this into SleepController's
  // `sleepAllowed` predicate and clears it when the chamber cools.
  bool overTempLatched() const { return overTemp_; }
  void clearOverTemp() { overTemp_ = false; }

  uint32_t linkTimeoutMs() const { return cfg_.linkTimeoutMs; }

private:
  void raise(uint32_t nowMs, uint32_t session, oven_FaultCode code) {
    const fault_table::FaultInfo info = fault_table::faultInfo(code);
    overTemp_ = overTemp_ || info.overTemp; // sticky across a later lower-priority update
    updatedAtMs_ = nowMs;
    ++count_;

    if (!active_) { // a new episode
      active_ = true;
      runActiveAtRaise_ = runActive_;
      raisedAtMs_ = nowMs;
      count_ = 1;
      code_ = code;
      session_ = session;
      severity_ = info.severity;
      return;
    }
    // Latched already: §22 — "update the overlay to the new cause and keep a +N count; don't
    // stack modals". Only a strictly higher severity takes over the displayed cause; the route
    // is never re-captured.
    if (fault_table::outranks(code, code_)) {
      code_ = code;
      session_ = session;
      severity_ = info.severity;
    }
  }

  Config cfg_;
  bool active_ = false;
  bool runActive_ = false;
  bool runActiveAtRaise_ = false;
  bool linkLostRaised_ = false;
  bool overTemp_ = false;
  oven_FaultCode code_ = oven_FaultCode_FAULT_NONE;
  uint32_t session_ = 0;
  uint32_t count_ = 0;
  fault_table::Severity severity_ = fault_table::Severity::RunIntegrity;
  uint32_t raisedAtMs_ = 0;
  uint32_t updatedAtMs_ = 0;
};
