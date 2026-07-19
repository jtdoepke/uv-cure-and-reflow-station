// oven_cal.h — the oven's plant constants for the planner/preview (design.md §6, §7; backlog B2).
//
// The design (§6/§7) mandates this file ultimately be *generated* by the offline calibration fit
// (D6·tools): PC-side least-squares over real telemetry → a human-reviewed, committed header →
// compiled identically into BOTH firmwares. That measured-data pipeline does not exist yet (and the
// donor may never run to produce it, teardown §0a), so the values below are still uncalibrated and
// CALIBRATED stays false — the §12 preview keeps its "estimated" labelling.
//
// They are, however, no longer arbitrary placeholders: they are the LINEARIZATION of the
// first-principles plant twin in lib/plant/oven_plant.h, whose PlantParams are anchored to the
// donor teardown (1500 W element, ~2000 J/K steel chamber, UA sized to the loss-limited ceiling).
// Deriving both the sim's forward model and this planning model from the SAME physics is what keeps
// the CYD's projected curve and the A10 bench sim in agreement (backlog A10; enforced by the
// cal-consistency host test). When D6·tools lands it regenerates this file from measured data; the
// thermal_math.h API and the OvenModel shape stay put, so downstream code is unaffected.
//
// Derivation (lumped model: dT/dt = (P·duty − UA·(T − Tamb)) / C_eff), with the oven_plant.h
// defaults P=1500 W, UA=5.9 W/K, Tamb=25 °C. During a ramp the element mass rises WITH the chamber,
// so the effective ramp mass is C_eff = chamberC + elementC = 2000 + 1000 = 3000 J/K (the calrod is
// part of what you heat); the steady-state ceiling P = UA·(T−Tamb) is mass-independent.
//   heat rate @ full duty : (P − UA·(T−Tamb))/C_eff = (P + UA·Tamb)/C_eff − (UA/C_eff)·T
//                           → slope −UA/C_eff = −0.00197, intercept 0.549  (the nonzero slope is
//                           the loss term — heating slows toward the ~279 °C ceiling, which a flat
//                           rate could not show). Fan-on delivers element heat faster → higher
//                           rate.
//   cool rate @ duty 0    :  UA·(T−Tamb)/C_eff = (UA/C_eff)·T − UA·Tamb/C_eff
//                           → slope +UA/C_eff = +0.00197, intercept −0.0492  (asymptotic cooling
//                           that slows toward ambient — replaces the old unphysical flat 0.2 °C/s).
//   holding duty duty_ss  :  UA·(T−Tamb)/P = (UA/P)·T − UA·Tamb/P  → slope 0.00393, intercept
//                           −0.0983 (mass-independent); rateGain = P/C_eff = 0.5 °C/s per unit
//                           duty.
//   board lag τ           :  the workpiece low-pass time constant (fan shortens it, §6).
// A cal-consistency host test pins these against the plant's measured rates so they cannot drift.
//
// NOT here (design boundary, §6): safety constants — per-mode hard-max, max-wall ceiling, trip
// thresholds — are NOT calibration deliverables. They live in the hand-written controller safety
// header (oven_safety.h) which the emitter can never write; the controller sanity-asserts these cal
// values against those compile-time bounds at boot. Nothing in this file governs safety.
//
// Pure constants: no LVGL, no Arduino. Consumed via the thermal_math.h math (B1/B3/B9/A5/C5/C7).
#pragma once

#include "thermal_math.h"

namespace oven_cal {

// False until the offline fit (D6·tools) regenerates this file from MEASURED data. Physics-anchored
// is still not measured, so consumers keep the "uncalibrated / estimated" preview labeling (§12).
constexpr bool CALIBRATED = false;

// --- Max heat/cool-rate envelopes (physics-anchored — see the derivation above) ----------------
// slope is now NONZERO: the loss term UA/C, so heating slows toward the ceiling and cooling slows
// toward ambient. floor guards the ETA integrand (rate never divides by ~0); ceiling caps the
// cold-end rate.
constexpr float RATE_FLOOR = 0.05f; // °C/s — minimum non-zero rate for the integrand

// Heating: faster with the convection fan on (better element→chamber delivery, §6).
constexpr RateEnvelope HEAT_FAN_OFF = {-0.00197f, 0.549f, RATE_FLOOR, 0.60f};
constexpr RateEnvelope HEAT_FAN_ON = {-0.00197f, 0.640f, RATE_FLOOR, 0.70f};

// Cooling: passive and one-sided — there is no chamber cool fan (§6), so cooling always uses the
// fan-off envelope. COOL_FAN_ON fills the FanPair's `.on` slot structurally but is never selected.
constexpr RateEnvelope COOL_FAN_OFF = {0.00197f, -0.0492f, RATE_FLOOR, 0.60f};
constexpr RateEnvelope COOL_FAN_ON = COOL_FAN_OFF; // unused (no cool fan, §6)

// --- First-order board-temp lag {a, b, τ} ------------------------------------------------------
// The convection fan shortens the board time constant (§6). a≈1, b≈0 = board ≈ wall until a real
// fit refines the affine terms.
constexpr LagParams LAG_FAN_OFF = {1.0f, 0.0f, 60.0f}; // τ ~60 s
constexpr LagParams LAG_FAN_ON = {1.0f, 0.0f, 30.0f};  // τ ~30 s

// --- Feedforward duty model duty_ss(T) + duty→rate gain ----------------------------------------
// Steady-state holding duty rises with target temperature (intercept < 0 → ~0 duty at ambient,
// clamped 0..1 by steadyStateDuty); rateGain is °C/s per unit duty (higher fan-on).
constexpr DutyModel DUTY_FAN_OFF = {0.003933f, -0.09833f, 0.50f};
constexpr DutyModel DUTY_FAN_ON = {0.003933f, -0.09833f, 0.60f};

// --- UV geometry (PLACEHOLDER — awaits the §6 405 nm photodiode coupon spin) --------------------
// beamCoverage defaults conservative-low so the exposure→hold conversion over-delivers dose and the
// §12 editor labels it "estimated" until measured (§6). turntableRpm only needs to be high enough
// that a hold spans many whole rotations (dose is RPM-independent, §6).
constexpr float BEAM_COVERAGE = 0.25f; // PLACEHOLDER, conservative-low
constexpr float TURNTABLE_RPM = 5.0f;  // PLACEHOLDER

// The single production plant model, assembled from the constants above. Downstream code takes this
// by const-ref; tests pass their own toy OvenModel instead.
//
// NOT named `DEFAULT`: this header compiles into both firmwares, and Arduino.h `#define`s DEFAULT
// (an analog-reference constant), so `oven_cal::DEFAULT` gets macro-expanded to `oven_cal::1` in
// any firmware TU that includes both — a syntax error the CYD firmware (C4) hit. Keep it
// macro-safe.
constexpr OvenModel kDefaultModel = {
    {HEAT_FAN_OFF, HEAT_FAN_ON}, // heat
    {COOL_FAN_OFF, COOL_FAN_ON}, // cool
    {LAG_FAN_OFF, LAG_FAN_ON},   // lag
    {DUTY_FAN_OFF, DUTY_FAN_ON}, // duty
    BEAM_COVERAGE,
    TURNTABLE_RPM,
    CALIBRATED,
};

} // namespace oven_cal
