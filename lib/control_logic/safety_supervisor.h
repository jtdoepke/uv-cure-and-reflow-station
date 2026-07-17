// SafetySupervisor — the controller's output safety gate (design.md §4, backlog A4a/A4b).
//
// The single owner of the heater's safety cutoff and the mains contactor. Every
// controller loop it re-derives, from the reliability layer, whether the oven may
// run *right now* and drives the outputs to match. It only ever forces OFF / opens;
// it never enables — the PID (A5) owns the heater duty, and this class has the last
// word over it (call tick() last in the loop).
//
// L1 run authorization is the composed term from §9 (ControllerLink::authorized()):
// handshake matched && active session && heartbeat fresh within the 750 ms command-
// timeout && the last heartbeat's enable bit. When that drops for any reason — a
// pulled CYD TX, a rebooted CYD, enable cleared, schema skew — the heater is forced
// OFF and the contactor opened on the very next tick. Fail-safe by construction:
// silence and staleness read as "stop", never as consent.
//
// L3 clamps (A4b) act on *measured* temperature, so they catch a welded SSR even when
// the control loop or the executor misbehave — the setpoint clamp alone cannot, since
// it only bounds what is *commanded*. While a run is armed and authorized, every tick:
//   - per-mode over-temp trip: a measured high-limit reading above hardMax[mode]+margin
//     opens the contactor (Fault{OVERTEMP_CHAMBER}). Independent of the control sensor.
//   - stuck-heater plausibility: measured temp rising while commanded duty ~ 0 across a
//     window means the SSR is welded on (Fault{HEATER_STUCK}).
//   - bounded total runtime: a run that outlives Σ(projected dur) × margin faults
//     (Fault{RUNTIME_EXCEEDED}).
//   - high-limit sensor fault: no usable high-limit channel while running is itself a
//     stop (Fault{SENSOR_FAULT}) — the supervisor refuses to run blind on safety.
// The high-limit reading is the max over the non-faulted wall channels: §4 wants an
// independent high-limit sensor on its *own* channel, and taking the hottest valid wall
// is conservative and needs no knowledge of which channel the executor uses for control.
// A watchdog reset from the previous boot is reported once via faultCode() as
// Fault{WATCHDOG} (noteResetCause) — the controller has already come up safe, so this is
// annunciation, not a latch.
//
// The setpoint clamp (§4) is exposed as clampSetpoint(): the caller routes the executor's
// setpoint through it *before* the PID (tick() runs after the PID, so it cannot clamp the
// PID's input). It is total — any input, including NaN/±Inf, yields a finite value in
// [0, hardMax]. Redundant with the executor's own clamp by design: defense-in-depth.
//
// Contactor policy (§4): energize-to-close, energized *only while a run is actively
// commanded*, i.e. only while authorized() && !faulted(). Idle, faulted, a lost link
// past the timeout, or enable=false all de-energize the coil, removing mains.
//
// A4b routes two fault sources into one latch: the executor's Output.fault (routed by the
// caller via trip(code)) and the L3 checks above (internal). faultCode() exposes the single
// active code for the controller's Fault-emit path (backlog A4b PR2). trip() is what makes a
// fault sticky — it holds the safe state until clearFault().
//
// Lives in control_logic (composes ControllerLink + HeaterActuator + IContactor +
// IThermocouples + IClock); all are injected by reference and must outlive this object.
// Header-only.
#pragma once

#include <cmath>
#include <cstdint>

#include "IClock.h"
#include "IContactor.h"
#include "IThermocouples.h"
#include "IWatchdog.h" // ResetCause — mapping it onto a FaultCode is A4b's job (IWatchdog.h)
#include "controller_link.h"
#include "heater_actuator.h"
#include "oven.pb.h"
#include "oven_safety.h"

class SafetySupervisor {
public:
  SafetySupervisor(ControllerLink &link, HeaterActuator &heater, IContactor &contactor,
                   IThermocouples &tc, IClock &clock)
      : link_(link), heater_(heater), contactor_(contactor), tc_(tc), clock_(clock) {
    // Fail-safe default before the first tick(): heater OFF, mains isolated.
    heater_.forceOff();
    contactor_.setClosed(false);
  }

  // Map the previous boot's reset cause onto a boot fault. A watchdog reset means the
  // loop hung last boot; report it once so the CYD annunciates Fault{WATCHDOG} (§9)
  // rather than the pair coming back silently. Not a latch: the controller is already
  // safe on this boot, so an authorized run may still proceed. Call from setup() after
  // the watchdog's begin(). Any non-watchdog cause is an ordinary boot — nothing to say.
  void noteResetCause(ResetCause cause) {
    if (cause == ResetCause::Watchdog && faultCode_ == oven_FaultCode_FAULT_NONE) {
      faultCode_ = oven_FaultCode_FAULT_WATCHDOG;
    }
  }

  // Arm the L3 checks for a run: derive the cap-selector mode from recipe *content* (never
  // the untrusted tag), cache the over-temp trip point, and budget the total runtime from
  // Σ(projected dur_ms) × margin. Call when a run is adopted (Start). disarmRun() on
  // Abort / Done / session end. Arming does not clear a latched fault.
  void armRun(const oven_Recipe &recipe) {
    const oven_Mode mode = oven_safety::deriveMode(recipe);
    hardMaxC_ = oven_safety::hardMaxForMode(mode);

    // Σ dur_ms in 64-bit so a raw/adversarial recipe cannot overflow the sum, then
    // saturate the budget to uint32 for wrap-safe elapsed comparison (any real run is far
    // under 49 days; a degenerate huge budget simply disables the runtime backstop, which
    // the per-segment watchdog already covers).
    uint64_t sum = 0;
    for (pb_size_t i = 0; i < recipe.segments_count; ++i) {
      sum += recipe.segments[i].dur_ms;
    }
    const uint64_t budget =
        static_cast<uint64_t>(static_cast<double>(sum) * oven_safety::RUNTIME_MARGIN_FRAC);
    runtimeBudgetMs_ = budget > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(budget);

    runStartMs_ = clock_.millis();
    const HighLimit hl = highLimit();
    stuckWinMs_ = runStartMs_;
    stuckWinTempC_ = hl.valid ? hl.celsius : 0.0F;
    armed_ = true;
  }

  // Release the L3 checks (run over). Leaves any latched fault in place.
  void disarmRun() { armed_ = false; }

  // Call every controller loop, last (after the PID sets duty and the actuator ticks), so
  // safety overrides whatever was commanded.
  void tick() {
    // L3 runs only while a run is armed and still authorized: an over-temp or welded SSR
    // matters while mains can be present, and the contactor is already open otherwise.
    if (armed_ && !faulted_ && link_.authorized()) {
      checkL3();
    }
    const bool run = link_.authorized() && !faulted_;
    setContactor(run);
    if (!run) {
      heater_.forceOff();
    }
    // run==true: leave the heater duty to the control loop; the supervisor only cuts.
  }

  // Independent hard-max clamp for the setpoint feeding the PID, applied by the caller
  // *before* HeaterControl::update (tick() runs after). Total: NaN/≤0 → 0, anything over
  // the ceiling → the ceiling. The ceiling is the armed mode's hard-max, or the reflow
  // (higher) ceiling when no run is armed — a clamp with no armed mode never tightens
  // below reflow, and the executor's own clamp is the real bound then.
  float clampSetpoint(float requested) const {
    const float hi = armed_ ? hardMaxC_ : oven_safety::REFLOW_HARD_MAX_C;
    if (!(requested > 0.0F)) {
      return 0.0F; // NaN and non-positive both land here
    }
    return requested > hi ? hi : requested;
  }

  // Latch the oven into the safe state (contactor open, heater off) until clearFault(),
  // recording the fault code the caller/L3 tripped on. The no-arg overload keeps A4a's
  // behavior (latch without a specific code -> INTERNAL as the catch-all).
  void trip(oven_FaultCode code) {
    faulted_ = true;
    faultCode_ = code;
    heater_.forceOff();
    setContactor(false);
  }
  void trip() { trip(oven_FaultCode_FAULT_INTERNAL); }

  // Clear the latch and any reported fault. Outputs stay safe until the next tick()
  // re-derives run.
  void clearFault() {
    faulted_ = false;
    faultCode_ = oven_FaultCode_FAULT_NONE;
  }

  bool faulted() const { return faulted_; }
  // The single active fault code for the controller's Fault-emit path (A4b PR2). FAULT_NONE
  // when neither a trip nor a boot annunciation is in effect.
  oven_FaultCode faultCode() const { return faultCode_; }
  // The oven is safe iff mains is isolated (the contactor is the last electrical line).
  bool safe() const { return !contactor_closed_; }

private:
  // A usable high-limit reading, or valid=false when no wall channel is trustworthy.
  struct HighLimit {
    float celsius;
    bool valid;
  };

  // The hottest non-faulted wall channel (§4's independent high-limit input). A faulted or
  // non-finite channel is never treated as a temperature; if every channel is unusable the
  // reading is invalid and the run must stop.
  HighLimit highLimit() const {
    float mx = 0.0F;
    bool any = false;
    const int n = tc_.wallCount();
    for (int i = 0; i < n; ++i) {
      const TcReading r = tc_.wall(i);
      if (!r.fault && std::isfinite(r.celsius) && (!any || r.celsius > mx)) {
        mx = r.celsius;
        any = true;
      }
    }
    return {mx, any};
  }

  void checkL3() {
    const uint32_t now = clock_.millis();

    // Bounded total runtime: a run that outlives its budget faults.
    if (runtimeBudgetMs_ > 0 && static_cast<uint32_t>(now - runStartMs_) > runtimeBudgetMs_) {
      trip(oven_FaultCode_FAULT_RUNTIME_EXCEEDED);
      return;
    }

    const HighLimit hl = highLimit();
    if (!hl.valid) {
      // No usable high-limit sensor while running: refuse to run blind on safety.
      trip(oven_FaultCode_FAULT_SENSOR_FAULT);
      return;
    }

    // Per-mode over-temp trip on measured temp — catches a welded SSR the setpoint clamp
    // cannot, because it acts on what the chamber actually reached.
    if (hl.celsius > hardMaxC_ + oven_safety::OVERTEMP_MARGIN_C) {
      trip(oven_FaultCode_FAULT_OVERTEMP_CHAMBER);
      return;
    }

    // Stuck-heater plausibility: temp climbing while commanded duty ~ 0 => welded SSR.
    // heater_.duty() is read here (in the last tick of the loop) as the duty just latched.
    if (heater_.duty() > oven_safety::STUCK_HEATER_DUTY_EPS) {
      // Heater legitimately on: a rise is expected, so re-anchor the window to now.
      stuckWinMs_ = now;
      stuckWinTempC_ = hl.celsius;
    } else if (static_cast<uint32_t>(now - stuckWinMs_) >= oven_safety::STUCK_HEATER_WINDOW_MS) {
      if (hl.celsius - stuckWinTempC_ >= oven_safety::STUCK_HEATER_RISE_C) {
        trip(oven_FaultCode_FAULT_HEATER_STUCK);
        return;
      }
      // A full quiet window passed without an implausible rise: start the next one.
      stuckWinMs_ = now;
      stuckWinTempC_ = hl.celsius;
    }
  }

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
  IThermocouples &tc_;
  IClock &clock_;

  bool faulted_ = false;
  oven_FaultCode faultCode_ = oven_FaultCode_FAULT_NONE;
  bool contactor_closed_ = false; // mirrors the constructor's fail-safe open

  bool armed_ = false;
  float hardMaxC_ = 0.0F;
  uint32_t runtimeBudgetMs_ = 0;
  uint32_t runStartMs_ = 0;

  uint32_t stuckWinMs_ = 0;    // stuck-heater window baseline time
  float stuckWinTempC_ = 0.0F; // stuck-heater window baseline temperature
};
