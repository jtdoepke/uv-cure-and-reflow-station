// SetpointShaper — the reference trajectory the PI loop actually tracks (design.md §5).
//
// The problem this exists for, found on the two-devkit bench (2026-07-19): a RAMP_ASAP segment
// makes ProfileExecutor emit its segment target as the setpoint *immediately*. The PI loop (A5)
// then sees a 30 °C error, saturates at full duty for the whole ramp, and overcharges the calrod —
// whose own thermal mass (oven_plant.h: elementC ≈ 1000 J/K, τ = C/g ≈ 50 s) keeps dumping heat
// into the chamber long after the SSR opens. A 60 °C cure peaked at 74.9 °C on the bench, matching
// the sim. Anti-windup cannot fix that: the integrator is not what saturates, the *proportional*
// term is, and the element's stored energy is already spent by the time the error reaches zero.
//
// design.md §5 says what to reach for: "The main reason to want D (limit peak overshoot) is better
// served by feedforward: the setpoint trajectory is fully known in advance (scripted profile) …
// Reach for feedforward before D." For an ASAP ramp the trajectory is precisely what is NOT known
// in advance — so this class *makes* it known, pacing the reference toward the target at the
// model's own achievable heat rate (thermal_math.h's envelopes — the same ones the CYD's ETA and
// §12 preview integrate, so the controller now tracks the trajectory the CYD projects).
//
// TWO limits, and the second is the one that actually kills the overshoot:
//   1. the envelope rate — the fastest the plant can go, from heatRate()/coolRate(); and
//   2. an APPROACH TAPER — rate ≤ remaining_distance / approachTauS, an exponential soft landing.
// Limit 1 alone would not help: the envelope *is* the saturated-duty rate, so pacing at it commands
// the same full duty as the step it replaced and charges the element just as hard. It is limit 2
// that eases the commanded rate — and with it the feedforward duty — down over the last stretch,
// so the element is already discharging when the chamber arrives. That is the backlog's "ease duty
// off *before* setpoint so the element isn't overcharged", expressed as a reference trajectory
// rather than as a fudge inside the PID.
//
// Consequences worth stating plainly:
//   - For a RAMP_OVER_TIME segment the executor's setpoint already moves slower than the envelope,
//     so pacing is TRANSPARENT — the input passes through. (The taper still applies over the last
//     approachTauS-ish of the sweep, which is the same soft landing, and is what keeps a timed ramp
//     from banging into its hold.)
//   - UPWARD APPROACHES ONLY. A falling setpoint passes straight through, because §5 is explicit
//     that "cool-down is passive (heater OFF + optional fan) → open-loop, not a PID-controlled
//     descent" — shaping a descent would mean holding the element partly on to track a cooling
//     curve, i.e. inventing the cooling controller the design says does not exist. It also keeps
//     the invariant below unconditionally true, which is worth more than a prettier cool ramp.
//   - The shaped setpoint is never ABOVE the executor's, so this can only ever reduce commanded
//     heat. It is not a safety element and must not be treated as one: SafetySupervisor::
//     clampSetpoint() runs over the result and safety_.tick() still has the last word (§4/§11).
//   - The executor's gating is untouched: reached() and the reflow hold-entry gate compare the
//     MEASURED temperature against the segment target, never against the setpoint the PI sees. So a
//     tapered reference cannot make a segment "arrive" early or late.
//   - It does not sanitize a non-finite setpoint or measurement. Those are blind control, and
//     HeaterControl::update() already treats them as a stop condition (OFF); laundering them into a
//     finite reference here would defeat that fail-safe. They pass straight through.
//
// The lead clamp (maxLeadC) is the anti-runaway backstop, and the reason this is safe to ship
// against an UNCALIBRATED model: if the envelope is optimistic and the chamber cannot keep up, the
// reference would otherwise sail away from the measurement and we would be back to a saturated step
// (plus a wound-up integrator). Clamping it to `measured + maxLeadC` bounds the error the PI can
// see, so an optimistic model degrades smoothly to today's behaviour instead of failing. It is
// deliberately generous — the reflow control sensor is the workpiece, which lags the chamber by
// design (§5/§6), and throttling that legitimate lag would stretch every reflow ramp.
//
// Header-only; depends only on IClock + thermal_math.h. Clock injected by reference, must outlive
// this object. No <Arduino.h> — native-compilable under native_control, and free of <cmath> like
// thermal_math.h itself.
#pragma once

#include <cstdint>

#include "IClock.h"
#include "thermal_math.h"

class SetpointShaper {
public:
  // ALL THREE are §10-OPEN placeholders, in the same sense as the PID gains and the executor's
  // watchdog constants: physics-anchored starting points, D7's to tune against a real oven.
  struct Config {
    // The soft-landing time constant: within `dist` of the target the reference may close at no
    // more than dist/approachTauS per second. Anchored to the element's own time constant
    // (oven_plant.h: elementC/gElemChamber ≈ 1000/20 = 50 s) — the reference must decelerate on the
    // timescale the element discharges on, or the stored heat still lands as overshoot.
    float approachTauS = 50.0f;
    // How far the reference may lead the measurement (anti-runaway only — see the header note).
    float maxLeadC = 40.0f;
    // Snap distance: once this close to the executor's setpoint, hand it through exactly, so
    // "arrived" is exact rather than asymptotic (the taper alone never quite lands).
    float arriveBandC = 0.5f;
    // Floor under the taper. The executor's per-segment watchdog judges a target-gated wait by the
    // MEASURED heat rate (ProfileExecutor::Config::rateFloorCPerS, 0.05 °C/s over a 30 s window),
    // and a plant tracking a tapered reference rises at the reference's rate — so a taper allowed
    // to decay below that floor makes a run that is arriving normally look stalled, and faults it
    // TARGET_UNREACHABLE. Found exactly that way in test_run_path. Kept comfortably above the
    // floor; run_path.h static_asserts the relationship so neither side can drift into the other.
    float minApproachRateCPerS = 0.15f;
  };

  // What the run path needs: the reference to track, and how fast it is moving — the feedforward
  // term's input (thermal_math.h rampFeedforwardDuty()).
  struct Shaped {
    float setpointC = 0.0f;
    float ratePerS = 0.0f;
  };

  SetpointShaper(IClock &clock, Config cfg) : clock_(clock), cfg_(cfg) {}
  explicit SetpointShaper(IClock &clock) : SetpointShaper(clock, Config{}) {}

  // Drop the reference → the next update() re-seeds at the measurement and paces nothing on that
  // tick (no dt baseline yet). Call on every stop/idle/fault alongside HeaterControl::reset(), so
  // no reference state leaks across runs.
  void reset() {
    haveRef_ = false;
    started_ = false;
    ref_ = 0.0f;
    rate_ = 0.0f;
  }

  // One tick. `spExec` is ProfileExecutor::Output.setpointC (already through clampSetpoint),
  // `measuredC` the mode's control sensor, `convFan` the segment's resolved fan state (the
  // envelopes are fan-conditioned, §6), `m` the plant model. dt comes from the injected clock.
  Shaped update(float spExec, float measuredC, bool convFan, const OvenModel &m) {
    const uint32_t now = clock_.millis();
    float dtS = 0.0f;
    if (started_) {
      dtS = static_cast<float>(now - lastMs_) / 1000.0f; // wrap-safe uint32 subtraction
    }
    started_ = true;
    lastMs_ = now;

    // Blind control (§4/A5): pass through untouched so HeaterControl's fail-safe sees the truth.
    if (!finite(spExec) || !finite(measuredC)) {
      haveRef_ = false;
      // Store what we return, non-finite and all, so reference() always reads back the last emitted
      // value rather than a stale one from before the fault. haveRef_ == false means the next tick
      // re-seeds from the measurement regardless, so nothing downstream ever paces off this.
      ref_ = spExec;
      rate_ = 0.0f;
      return Shaped{spExec, 0.0f};
    }

    // First tick of a run: the reference starts where the oven actually IS, so the ramp is paced
    // from the real starting temperature rather than from wherever the last run left off. Never
    // above the target — a chamber already hotter than the segment asks (the §15 cure resume) must
    // not be handed a reference that commands extra heat.
    if (!haveRef_) {
      ref_ = measuredC < spExec ? measuredC : spExec;
      haveRef_ = true;
      rate_ = 0.0f;
      return Shaped{ref_, 0.0f};
    }

    const float prev = ref_;
    advance(spExec, convFan, m, dtS);
    applyLeadClamp(measuredC, spExec);

    rate_ = dtS > 0.0f ? (ref_ - prev) / dtS : 0.0f;
    if (!finite(rate_)) {
      rate_ = 0.0f;
    }
    return Shaped{ref_, rate_};
  }

  // The reference as of the last update() — always exactly what that call returned. Diagnostic
  // (telemetry reports the EXECUTOR's setpoint, see run_path.h); nothing safety-critical reads it.
  float reference() const { return ref_; }
  float ratePerS() const { return rate_; }

private:
  // Move the reference toward spExec by this tick's allowance.
  void advance(float spExec, bool convFan, const OvenModel &m, float dtS) {
    // A falling (or reached) setpoint passes through: cool-down is open-loop by design (§5), and
    // pacing a descent would keep the heater on to follow it.
    if (spExec <= ref_) {
      ref_ = spExec;
      return;
    }
    if (!(dtS > 0.0f) || !finite(dtS)) {
      return; // no time passed (or a garbage dt) — hold the reference where it is
    }
    const float dist = spExec - ref_;
    if (dist <= cfg_.arriveBandC) {
      ref_ = spExec; // arrived
      return;
    }

    // Limit 1: the plant's own envelope, at the reference's own temperature (fan-conditioned, §6).
    const float envRate = heatRate(m, ref_, convFan);
    if (!finite(envRate) || envRate <= 0.0f) {
      // The model cannot say how fast this plant moves. Refusing to heat would be the wrong
      // failure — fall back to the unshaped setpoint, i.e. exactly the behaviour that shipped
      // before this class existed, and let the PI carry the ramp as it did then.
      ref_ = spExec;
      return;
    }
    // Limit 2: the approach taper — the soft landing that actually cuts the overshoot.
    float rate = envRate;
    const float tau = finiteOr(cfg_.approachTauS, 0.0f);
    if (tau > 0.0f) {
      float taper = dist / tau;
      // …but never slower than the executor's stall floor (see Config::minApproachRateCPerS), and
      // never faster than the plant can go — if the envelope itself is below the floor, the
      // envelope wins, because inventing rate the oven does not have would only move the stall.
      const float minRate = finiteOr(cfg_.minApproachRateCPerS, 0.0f);
      if (finite(taper) && taper < minRate) {
        taper = minRate;
      }
      if (finite(taper) && taper < rate) {
        rate = taper;
      }
    }

    const float step = rate * dtS;
    if (!finite(step) || step <= 0.0f) {
      return;
    }
    if (step >= dist - cfg_.arriveBandC) {
      ref_ = spExec; // this tick closes the gap (a coarse tick, or a clock jump)
      return;
    }
    ref_ += step;
  }

  // Bound how far the reference may lead the measurement, then re-assert the two invariants the
  // run path advertises: never above the executor's setpoint, always finite.
  void applyLeadClamp(float measuredC, float spExec) {
    const float lead = measuredC + finiteOr(cfg_.maxLeadC, 0.0f);
    if (finite(lead) && ref_ > lead) {
      ref_ = lead;
    }
    if (ref_ > spExec) {
      ref_ = spExec;
    }
    if (!finite(ref_)) {
      ref_ = spExec;
    }
  }

  // Finite guard without <cmath>, matching thermal_math.h / oven_plant.h. The ±3.4e38 bound is a
  // hair INSIDE FLT_MAX (3.4028e38) on purpose: a value that passes this cannot overflow on the
  // next multiply/subtract, which is the property the pacing math actually needs. So "finite" here
  // is very slightly stricter than std::isfinite — anything in that sliver takes the blind-control
  // path, which is the safe direction.
  static bool finite(float v) { return v == v && v <= 3.4e38f && v >= -3.4e38f; }
  static float finiteOr(float v, float fallback) { return finite(v) ? v : fallback; }

  IClock &clock_;
  Config cfg_;

  float ref_ = 0.0f;     // the shaped reference
  float rate_ = 0.0f;    // its rate of change, °C/s (the feedforward input)
  bool haveRef_ = false; // false until seeded at the measurement
  uint32_t lastMs_ = 0;
  bool started_ = false;
};
