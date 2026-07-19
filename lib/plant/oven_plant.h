// oven_plant.h — a first-principles thermal-plant simulator of the donor oven (design.md §5/§6,
// backlog A10).
//
// This is the "fake oven" the controller talks to when no real oven exists: it consumes the
// commanded outputs (heater duty 0..1, convection fan, UV, motor) and produces the temperatures
// a thermocouple would read, closing the control loop on the bench (A8) before the mains hardware
// (D3/D4) is built. Unlike thermal_math.h — which is *rate-space kinematics* for planning (how
// fast a ramp CAN go) — this is a *forward energy model*: given a duty over dt, what temperature
// results. The two are reconciled by oven_cal.h being back-fit from this model's linearization, so
// the CYD's projected curve and the sim agree (the A10 requirement; see the cal-consistency test).
//
// Lumped-capacitance nodes (the standard reflow-oven modelling approach — FOPDT / thermal-RC):
//   element  — the calrod's own thermal mass. Electrical power P·duty heats it; it transfers to the
//              chamber through a fan-conditioned conductance. Its lag is the post-shutoff overshoot
//              source the design's feedforward/anti-windup exist to tame (§5).
//   chamber  — the wall/air lump (fan-mixed, so wall temp is a good air proxy, §6). Receives from
//              the element + a small UV fraction; loses heat as UA·(T−Tamb). That UA·ΔT loss is
//              what gives a *loss-limited temperature ceiling* (steady state where P = UA·(T−Tamb),
//              so T_ceiling ≈ Tamb + P/UA ≈ 279 °C) and *asymptotic cooling* — the real oven's
//              defining behaviour, which a flat-rate model cannot express (the donor is "modest…
//              ramp rate is the binding constraint"). The sink is treated as ambient because the
//              always-on cooling fan keeps the bay ventilated toward room air; the bay node below
//              is a derived *indicator*, deliberately NOT in series with this loss (a series bay
//              would raise the ceiling well past the physical oven — and past the reflow hard-max).
//   workpiece— a first-order lag of the chamber (the §6 {a,b,τ} board estimator, run *forward*):
//              the reflow control sensor lags the chamber air. Reuses thermal_math.h lpStep.
//   bay      — the electronics bay the always-on cooling fan circulates (§6). Absorbs a fraction of
//              the chamber's shed heat plus most of the UV array's 60 W (the array sits outside the
//              cavity in this airflow) and sheds to room ambient. A readout only — what the §6
//              enclosure/TMP102 sensor sees — it does not feed back into the chamber's cooling.
//
// Physical anchors (donor teardown): P=1500 W @ 120 V (measured 9.6 Ω element); cavity
// 14.5×16.4×9.3 in of thin uninsulated steel → C≈2000 J/K → cold ramp P/C≈0.75 °C/s; UA chosen so
// the loss-limited ceiling sits ~280 °C (must exceed the 218 °C *firmware* cap — the element drives
// hotter). Every number is a NAMED, ADJUSTABLE PlantParams field: physics-anchored estimates, not
// measured calibration (the donor is likely faulty), refined later by D7 or a real heat-up run.
//
// SAFETY NOTE: this fabricates sensor readings — it exists only for the bench sim build
// (CONTROL_SIM) and host tests. It governs nothing on a real oven.
//
// Pure C++: no LVGL, no Arduino, and — matching thermal_math.h — no <cmath> (finiteness is checked
// by hand). Board-agnostic and host-testable under native_control; only the SimThermocouples
// injection adapter (src_control) is firmware-side.
#pragma once

#include <cstdint>

#include "thermal_math.h" // clampf, lpStep, boardTempEstimate, LagParams (reused forward)

// Units: temperature °C, time seconds, power W, thermal mass J/K, conductance W/K, duty 0..1.
// float everywhere (the ESP32 has hardware float), matching thermal_math.h.

// The oven's physical constants, as an adjustable bundle. Defaults are the teardown-anchored
// estimates documented above; tests/CLI pass their own. Fan-conditioned quantities carry off/on
// variants because the convection fan changes the plant (§6) — here it raises the element→chamber
// conductance (faster heat delivery) and shortens the workpiece lag; the *external* loss (UA) is
// set by the always-on cooling fan and is fan-state-independent, which is why both fan states share
// one loss-limited ceiling but fan-on reaches it faster.
struct PlantParams {
  float heaterWatts = 1500.0f; // element electrical power at full duty (measured 9.6 Ω @ 120 V)

  // Element node.
  float elementC = 1000.0f;      // J/K — calrod sheath thermal mass (sets post-shutoff overshoot)
  float gElemChamberOff = 20.0f; // W/K — element→chamber conductance, convection fan OFF
  float gElemChamberOn = 36.0f;  // W/K — … fan ON (forced air delivers element heat faster)

  // Chamber node (wall/air lump).
  float chamberC = 2000.0f; // J/K — steel walls + air + rack (cold ramp ≈ P/chamberC)
  float uaChamberLoss =
      5.9f; // W/K — chamber→ambient loss; sets the ceiling ≈ Tamb + P/UA (~279 °C)
  float insulationFactor = 1.0f; // ×uaChamberLoss; <1 models the §6 "considered" cavity insulation

  // Workpiece node (first-order lag of the chamber — the §6 {a,b,τ} estimator, forward).
  float tauWorkOff = 60.0f; // s — board lag, convection fan OFF (matches oven_cal LAG τ)
  float tauWorkOn = 30.0f;  // s — … fan ON (fan shortens the board time constant, §6)
  float workGain = 1.0f;    // a — affine gain of boardTempEstimate
  float workOffsetC = 0.0f; // b — affine offset

  // Bay node (electronics bay the always-on cooling fan circulates, §6) — a readout, not in series
  // with the chamber loss.
  float bayC = 800.0f;          // J/K — bay air + chassis mass
  float uaBayAmbient = 15.0f;   // W/K — bay→room, driven by the always-on cooling fan (16 W)
  float bayHeatFraction = 0.6f; // fraction of the chamber's shed heat the bay air picks up

  // UV array (405 nm / 60 W, outside the cavity behind a window in the bay airflow, §6): most of
  // its heat leaves via the bay, only a small radiant fraction enters the chamber.
  float uvWatts = 60.0f;
  float uvChamberFraction = 0.1f; // fraction of UV power reaching the chamber (rest → bay)

  float ambientC = 25.0f; // room/starting temperature
};

// The forward plant. Construct with params + starting (ambient) temperature; step() every control
// loop with the commanded outputs; read the synthetic temperatures back through the accessors. All
// outputs are always finite and clamped to a generous physical band, so an adversarial caller
// (fuzz: NaN/Inf duty, huge/negative dt) can never produce a non-finite reading.
class OvenPlant {
public:
  explicit OvenPlant(PlantParams p = PlantParams{})
      : p_(p), elemC_(p.ambientC), chamberC_(p.ambientC), workLp_(p.ambientC), bayC_(p.ambientC) {}

  // Reset all nodes to ambient (a fresh run). Params are unchanged.
  void reset() {
    elemC_ = p_.ambientC;
    chamberC_ = p_.ambientC;
    workLp_ = p_.ambientC;
    bayC_ = p_.ambientC;
  }

  // Advance the plant by dtS seconds under the commanded outputs. `appliedDuty` is the
  // POST-safety heater duty (HeaterActuator::duty() read after SafetySupervisor::tick()) — the
  // average power fraction actually applied, which is exactly what a lumped model integrates.
  // motor has no thermal effect (turntable). Robust to non-finite / huge / non-positive dt.
  void step(float dtS, float appliedDuty, bool convFan, bool uv, bool /*motor*/) {
    const float duty = clampf(finiteOr(appliedDuty, 0.0f), 0.0f, 1.0f);
    float t = finiteOr(dtS, 0.0f);
    if (t <= 0.0f) {
      return; // no time passed (or a garbage dt) — nothing to integrate
    }
    if (t > kMaxStepS) {
      t = kMaxStepS; // real loops pass ms..1 s; cap keeps a pathological dt bounded
    }
    // Sub-step so explicit Euler stays stable (τ_element ~20 s ≫ a sub-step), with a hard cap on
    // the sub-step count so a huge dt cannot blow up the loop cost.
    int n = static_cast<int>(t / kSubStepS) + 1;
    if (n > kMaxSubSteps) {
      n = kMaxSubSteps;
    }
    const float h = t / static_cast<float>(n);
    for (int i = 0; i < n; ++i) {
      integrate(h, duty, convFan, uv);
    }
  }

  // Synthetic sensor readings (all finite, all clamped to the physical band).
  float chamberTempC() const { return chamberC_; } // the wall/air lump
  float wallTempC() const { return chamberC_; }    // walls ≈ fan-mixed air (§6)
  float workpieceTempC() const {                   // the lagged board (reflow sensor)
    return clampT(boardTempEstimate(LagParams{p_.workGain, p_.workOffsetC, 0.0f}, workLp_));
  }
  float bayTempC() const { return bayC_; }      // electronics bay (§6 ambient sensor)
  float elementTempC() const { return elemC_; } // element sheath (debug/plotting)

private:
  // One explicit-Euler sub-step over h seconds. Uses the current node values for all derivatives
  // (standard forward Euler), then clamps each node to the physical band so no NaN/Inf escapes.
  void integrate(float h, float duty, bool convFan, bool uv) {
    const float gEC = convFan ? p_.gElemChamberOn : p_.gElemChamberOff;
    const float ua = p_.uaChamberLoss * clampf(finiteOr(p_.insulationFactor, 1.0f), 0.05f, 1.0f);
    const float tauWork = convFan ? p_.tauWorkOn : p_.tauWorkOff;
    const float uvW = finiteOr(p_.uvWatts, 0.0f) * (uv ? 1.0f : 0.0f);

    const float pElem = finiteOr(p_.heaterWatts, 0.0f) * duty; // W into the element
    const float qElemChamber = gEC * (elemC_ - chamberC_);     // W element→chamber
    const float qChamberLoss = ua * (chamberC_ - p_.ambientC); // W chamber→ambient (the ceiling)
    const float qBayAmbient = p_.uaBayAmbient * (bayC_ - p_.ambientC); // W bay→room
    const float uvChamber = uvW * clampf(p_.uvChamberFraction, 0.0f, 1.0f);
    const float uvBay = uvW - uvChamber;
    // The bay picks up a fraction of the chamber's shed heat (fan blowing over the hot exterior)
    // plus most of the UV array's dissipation; it is an indicator and does NOT reduce qChamberLoss.
    const float qBayIn = clampf(p_.bayHeatFraction, 0.0f, 1.0f) * qChamberLoss + uvBay;

    const float dElem = (pElem - qElemChamber) / posC(p_.elementC);
    const float dChamber = (qElemChamber + uvChamber - qChamberLoss) / posC(p_.chamberC);
    const float dBay = (qBayIn - qBayAmbient) / posC(p_.bayC);

    elemC_ = clampT(elemC_ + finiteOr(dElem, 0.0f) * h);
    chamberC_ = clampT(chamberC_ + finiteOr(dChamber, 0.0f) * h);
    bayC_ = clampT(bayC_ + finiteOr(dBay, 0.0f) * h);
    // Workpiece: first-order low-pass of the chamber (reuse thermal_math.h). tau<=0 → tracks.
    workLp_ = clampT(lpStep(workLp_, chamberC_, h, tauWork));
  }

  // Finite guard without <cmath>: v==v is false only for NaN; the range rejects ±Inf.
  static float finiteOr(float v, float fallback) {
    return (v == v && v <= 3.4e38f && v >= -3.4e38f) ? v : fallback;
  }
  // Keep every node in a generous physical band (well past any real oven, well short of overflow),
  // so a pathological input clamps rather than diverges. Also collapses a NaN to ambient-ish.
  static float clampT(float v) { return clampf(finiteOr(v, kTempMin), kTempMin, kTempMax); }
  // A thermal mass that can't be zero/negative (would divide by ~0); floor it.
  static float posC(float c) { return c > 1.0f ? c : 1.0f; }

  static constexpr float kTempMin = -100.0f;  // below any ambient
  static constexpr float kTempMax = 5000.0f;  // far past a glowing element, far short of overflow
  static constexpr float kMaxStepS = 3600.0f; // cap on a single step (real loops: ms..1 s)
  static constexpr float kSubStepS = 0.5f;    // Euler sub-step (τ_element ~20 s ≫ this)
  static constexpr int kMaxSubSteps = 256;    // bound the sub-step loop under a huge dt

  PlantParams p_;
  float elemC_;    // element sheath temperature
  float chamberC_; // chamber wall/air temperature
  float workLp_;   // workpiece low-pass state (pre-affine)
  float bayC_;     // electronics-bay temperature
};
