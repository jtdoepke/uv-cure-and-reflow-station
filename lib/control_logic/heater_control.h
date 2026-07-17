// HeaterControl — the closed-loop heater controller (design.md §5, backlog A5).
//
// Closes the loop the rest of the controller leaves open: given the moving setpoint the
// ProfileExecutor (A6) emits and the mode's measured control sensor (IThermocouples, §5/§6
// — workpiece in reflow, wall in cure), it produces the 0..1 duty HeaterActuator::setDuty()
// time-proportions onto the zero-cross SSR. A6 *emits* the setpoint; A5 *tracks* it; the two
// are built and tested independently (§5).
//
// PI, not PID (design.md §5). The plant is slow, lag- and dead-time-dominated, so PI handles
// it well and D mostly amplifies the quantized-thermocouple + SSR-switching noise. Kd is kept
// as an inert seam (defaults 0, derivative-**on-measurement** so a setpoint step can't kick it,
// low-passable via dMeasTauS) so D is a drop-in later if peak overshoot proves stubborn — it
// contributes exactly nothing while Kd == 0.
//
// Anti-windup is mandatory (§5): the actuator saturates (duty 0..1) with dead time, so the
// integrator winds up on every ramp unless bounded. This uses **conditional integration** —
// the integrator only accumulates while doing so keeps the duty in range, or while it would
// unwind an existing saturation — which needs no extra back-calculation gain and is robust
// without knowing the actuator's internals (HeaterActuator just clamps; it never reports
// saturation back). The heater is one-sided (heat only), so overshoot drives duty to 0 and
// bleeds the integrator down; cool-down is passive/open-loop and not this loop's job.
//
// Feedforward is a caller-supplied hook (§5/§6): update() takes a feedforward duty and adds
// it to the output, with anti-windup accounting for it, so feedback only trims the residual.
// The caller computes it from the inverse-plant model — steadyStateDuty() in
// lib/calibration/thermal_math.h — keeping this class free of any calibration dependency, the
// same way the executor stays OvenModel-free. Pass 0 for pure feedback.
//
// Fail-safe by construction (§4): a non-finite setpoint or measurement is "blind control", a
// stop condition — update() commands OFF and freezes the integrator. The caller additionally
// calls reset() whenever the run isn't live (executor Output.safe, a faulted TC channel, or a
// de-authorized link) so the integrator never carries accumulation across a stop. The
// SafetySupervisor (§4) still has the last word over the duty this produces.
//
// Header-only; depends only on IClock. Clock injected by reference, must outlive this object.
// Keep free of <Arduino.h> so it stays native-compilable under native_control.
#pragma once

#include <cmath>
#include <cstdint>

#include "IClock.h"

class HeaterControl {
public:
  // PI gains (+ the inert D seam). Placeholders below are §10-open — real values are a
  // bench-tuning deliverable against a live oven (like the executor's watchdog constants).
  struct Gains {
    float kp = 0.02f;  // duty per °C error (≈ full duty at 50 °C error)
    float ki = 0.002f; // duty per (°C·s) — integral rate
    float kd = 0.0f;   // duty per (°C/s); the §5 seam, off by default
  };

  struct Config {
    Gains gains{};
    float dutyMin = 0.0f;   // one-sided heater: never commands "cooling"
    float dutyMax = 1.0f;   // full duty
    float dMeasTauS = 0.0f; // low-pass τ (s) for the D-on-measurement term (§5 "low-passed
                            // hard"); 0 = unfiltered, and inert entirely while kd == 0
  };

  HeaterControl(IClock &clock, Config cfg) : clock_(clock), cfg_(cfg) {}

  // Convenience overload with default Config (a `Config cfg = Config{}` default argument would
  // reference Config's member initializers before the class is complete — see HeaterActuator).
  explicit HeaterControl(IClock &clock) : HeaterControl(clock, Config{}) {}

  // Drop all accumulated state → next update() starts fresh (dt baseline re-established, so it
  // does no integration on that first tick). Call on any stop/idle/fault so the integrator and
  // derivative history never leak across runs.
  void reset() {
    integrator_ = 0.0f;
    duty_ = 0.0f;
    started_ = false;
    haveMeas_ = false;
    filtMeas_ = 0.0f;
  }

  // One control tick. `setpointC` is ProfileExecutor::Output.setpointC; `measuredC` is the
  // mode's control sensor; `feedforwardDuty` is the caller's inverse-plant term (0 for none).
  // Returns the duty for HeaterActuator::setDuty(), always finite and within [dutyMin, dutyMax].
  // dt is derived from the injected clock.
  float update(float setpointC, float measuredC, float feedforwardDuty) {
    const uint32_t now = clock_.millis();
    float dtS = 0.0f;
    if (started_) {
      dtS = static_cast<float>(now - lastMs_) / 1000.0f; // wrap-safe uint32 subtraction
    }
    started_ = true;
    lastMs_ = now;

    // Fail-safe: blind control (non-finite setpoint/measured, or an error that overflows two
    // large finites) is a stop condition — OFF, integrator held, derivative history dropped.
    const float error = setpointC - measuredC;
    if (!std::isfinite(setpointC) || !std::isfinite(measuredC) || !std::isfinite(error)) {
      duty_ = 0.0f;
      haveMeas_ = false;
      return duty_;
    }

    const Gains &g = cfg_.gains;
    const float ff = std::isfinite(feedforwardDuty) ? feedforwardDuty : 0.0f;

    // Derivative-on-measurement (design.md §5), optionally low-passed. Inert while kd == 0.
    float measForD = measuredC;
    if (cfg_.dMeasTauS > 0.0f && dtS > 0.0f && haveMeas_) {
      const float alpha = dtS / (cfg_.dMeasTauS + dtS);
      measForD = filtMeas_ + alpha * (measuredC - filtMeas_);
    }
    float dTerm = 0.0f;
    if (g.kd != 0.0f && dtS > 0.0f && haveMeas_) {
      const float dMeas = (measForD - filtMeas_) / dtS;
      if (std::isfinite(dMeas)) {
        dTerm = -g.kd * dMeas;
      }
    }
    filtMeas_ = measForD;
    haveMeas_ = true;

    const float pBase = ff + g.kp * error + dTerm; // everything but the integral term

    // Anti-windup: conditional integration. Add error·dt to the integrator only if the
    // resulting duty stays in range, or if integrating pulls it back out of saturation.
    const float iCandidate = integrator_ + error * dtS;
    if (std::isfinite(iCandidate)) {
      const float rawCand = pBase + g.ki * iCandidate;
      const bool inRange =
          std::isfinite(rawCand) && rawCand >= cfg_.dutyMin && rawCand <= cfg_.dutyMax;
      const bool unwinding =
          (rawCand > cfg_.dutyMax && error < 0.0f) || (rawCand < cfg_.dutyMin && error > 0.0f);
      if (inRange || unwinding) {
        integrator_ = iCandidate;
      }
    }
    // Hard bound so the integrator stays finite even under adversarial gains/inputs (a fuzz
    // invariant) — far above any real value (≈ dutyMax/ki), far below float overflow.
    integrator_ = clamp(integrator_, -kIntegratorMax, kIntegratorMax);

    const float raw = pBase + g.ki * integrator_;
    // A non-finite raw (Inf from huge gains, or NaN from +Inf/−Inf cancellation) → safe OFF;
    // otherwise clamp to the actuator's duty range. clamp() never returns NaN for finite raw.
    duty_ = std::isfinite(raw) ? clamp(raw, cfg_.dutyMin, cfg_.dutyMax) : 0.0f;
    return duty_;
  }

  float duty() const { return duty_; }
  float integrator() const { return integrator_; }

private:
  static float clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

  static constexpr float kIntegratorMax = 1.0e9f;

  IClock &clock_;
  Config cfg_;

  float integrator_ = 0.0f;
  float duty_ = 0.0f;
  uint32_t lastMs_ = 0;
  bool started_ = false;

  float filtMeas_ = 0.0f; // last (optionally low-passed) measurement, for the D seam
  bool haveMeas_ = false; // false until the first valid measurement — no derivative yet
};
