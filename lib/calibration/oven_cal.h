// oven_cal.h — the oven's calibrated plant constants (design.md §6, §7; backlog B2).
//
// The design (§6/§7) mandates this file be *generated* by the offline calibration fit (D6·tools):
// PC-side least-squares over telemetry logs → a human-reviewed, committed header → compiled
// identically into BOTH firmwares (one source, both binaries the same by construction). That
// pipeline does not exist yet, so every value below is a HAND-AUTHORED PLACEHOLDER, and
// CALIBRATED is false. When D6·tools lands it regenerates this file; the thermal_math.h API and
// the OvenModel shape stay put, so downstream code is unaffected.
//
// Because CALIBRATED is false, the constant-rate (slope 0) envelopes here make every preview an
// idealized straight line — exactly the "uncalibrated — preview is idealized" behavior §12
// specifies for a fresh oven. The numbers are order-of-magnitude sane, NOT authoritative.
//
// NOT here (design boundary, §6): safety constants — per-mode hard-max, max-wall ceiling, trip
// thresholds — are NOT calibration deliverables. They live in the hand-written controller safety
// header (A4b/A7) which the emitter can never write; the controller sanity-asserts these cal
// values against those compile-time bounds at boot. Nothing in this file governs safety.
//
// Pure constants: no LVGL, no Arduino. Consumed via the thermal_math.h math (B1/B3/B9/A5/C5/C7).
#pragma once

#include "thermal_math.h"

namespace oven_cal {

// False until the offline fit (D6·tools) regenerates this file. Consumers key the "uncalibrated /
// estimated" preview labeling (§12) off this.
constexpr bool CALIBRATED = false;

// --- Max heat/cool-rate envelopes (PLACEHOLDER) ------------------------------------------------
// slope 0 → constant rate → idealized-linear preview (§12). Heat "fan on" models the convection
// fan helping (§6 "convection fan shortens the board time constant"). floor guards the ETA
// integrand; ceiling equals the constant rate here.
constexpr float RATE_FLOOR = 0.05f; // °C/s, PLACEHOLDER — minimum non-zero rate for the integrand

// Heating: faster with the convection fan on.
constexpr RateEnvelope HEAT_FAN_OFF = {0.0f, 0.8f, RATE_FLOOR, 0.8f}; // PLACEHOLDER ~0.8 °C/s
constexpr RateEnvelope HEAT_FAN_ON = {0.0f, 1.2f, RATE_FLOOR, 1.2f};  // PLACEHOLDER ~1.2 °C/s

// Cooling: passive and one-sided — there is no chamber cool fan (§6), so cooling always uses the
// fan-off envelope. COOL_FAN_ON fills the FanPair's `.on` slot structurally but is never selected;
// keep it a copy of the passive rate so a stray read can't imply a nonexistent fan helps.
constexpr RateEnvelope COOL_FAN_OFF = {0.0f, 0.2f, RATE_FLOOR, 0.2f}; // PLACEHOLDER ~0.2 °C/s
constexpr RateEnvelope COOL_FAN_ON = COOL_FAN_OFF;                    // unused (no cool fan, §6)

// --- First-order board-temp lag {a, b, τ} (PLACEHOLDER) ----------------------------------------
// The convection fan shortens the board time constant (§6). a≈1, b≈0 = board ≈ wall until fit.
constexpr LagParams LAG_FAN_OFF = {1.0f, 0.0f, 60.0f}; // PLACEHOLDER τ ~60 s
constexpr LagParams LAG_FAN_ON = {1.0f, 0.0f, 30.0f};  // PLACEHOLDER τ ~30 s

// --- Feedforward duty model duty_ss(T) + duty→rate gain (PLACEHOLDER) --------------------------
// Steady-state holding duty rises with target temperature; rateGain is °C/s per unit duty.
constexpr DutyModel DUTY_FAN_OFF = {0.0020f, 0.05f, 0.9f}; // PLACEHOLDER
constexpr DutyModel DUTY_FAN_ON = {0.0025f, 0.05f, 1.3f};  // PLACEHOLDER

// --- UV geometry (PLACEHOLDER) -----------------------------------------------------------------
// beamCoverage defaults conservative-low so the exposure→hold conversion over-delivers dose and
// the §12 editor labels it "estimated" until measured (§6). turntableRpm only needs to be high
// enough that a hold spans many whole rotations (dose is RPM-independent, §6).
constexpr float BEAM_COVERAGE = 0.25f; // PLACEHOLDER, conservative-low
constexpr float TURNTABLE_RPM = 5.0f;  // PLACEHOLDER

// The single production plant model, assembled from the placeholders above. Downstream code takes
// this by const-ref; tests pass their own toy OvenModel instead.
//
// NOT named `DEFAULT`: this header compiles into both firmwares, and Arduino.h `#define`s DEFAULT
// (an analog-reference constant), so `oven_cal::DEFAULT` gets macro-expanded to `oven_cal::1` in
// any firmware TU that includes both — a syntax error the CYD firmware (C4, the first CYD consumer
// of this header) hit. `kDefaultModel` sidesteps the macro; keep it macro-safe.
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
