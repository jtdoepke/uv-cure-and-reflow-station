// Internal-property harness: the PI heater control loop (A5, lib/control_logic/heater_control.h)
// must keep its output safe under an arbitrary gain set AND an adversarial signal trajectory.
//
// The headline property is an **output-safety invariant**: whatever sequence of setpoints,
// measurements, feedforward terms, gains, and clock steps the fuzzer supplies — including NaN /
// Inf / denormal floats, extreme gains, and zero or huge dt — the commanded duty is ALWAYS
// finite and within [0, dutyMax]. That duty physically drives a heater, so a NaN or an
// out-of-range value escaping the loop is a safety bug, not a cosmetic one (HeaterActuator's
// setDuty() NaN-guards precisely because this class's math is where it could originate). A
// hand-written test can only try a handful of trajectories; coverage-guided mutation with UBSAN/
// ASAN pushes the division-by-dt, the integrator accumulation, and the float→duty clamp at their
// boundaries.
//
// Secondary invariants: the integrator stays finite and bounded (anti-windup must actually hold,
// whatever the gains), a non-finite setpoint/measurement fails safe to duty 0, and reset() clears
// the integrator and duty to 0.
//
// Like fuzz_executor this is an invariant harness (no oracle): it drives a real HeaterControl over
// a FakeClock and asserts postconditions every tick.
//
// Input format (documented so seed_gen.cpp can emit an on-contract seed; little-endian):
//   header, 20 bytes:
//     [0..3]   kp        : float32 (raw → NaN/Inf/denormal/extreme)
//     [4..7]   ki        : float32 (raw)
//     [8..11]  kd        : float32 (raw — exercises the derivative seam)
//     [12..15] dMeasTauS : float32 (raw — the D low-pass τ; ≤0/non-finite disables the filter)
//     [16..19] dutyMax   : float32; constrained to (0,1] (else 1.0) so the range invariant is
//                          well-defined. dutyMin is fixed at 0 (production: the heater is
//                          one-sided).
//   then N × 15-byte tick records, consumed until the bytes run out:
//     [0..3]   setpointC : float32 (raw)
//     [4..7]   measuredC : float32 (raw; non-finite drives the fail-safe path)
//     [8..11]  ffDuty    : float32 (raw feedforward term)
//     [12..13] clockStep : uint16; the tick advances the clock by this many ms (0..65535, incl. 0)
//     [14]     flags     : bit0 → reset() before this tick
#include <cmath>
#include <cstring>

#include "fuzz_util.h"

#include "heater_control.h"
#include "helpers/fake_clock.h" // -I test (fuzz_common); reuse the same fake the unit tests use

namespace {

constexpr size_t kHeaderBytes = 20;
constexpr size_t kTickRecord = 15;
// Anti-windup bounds the integrator to HeaterControl::kIntegratorMax (1e9); allow a hair over.
constexpr float kIntegratorBound = 1.0e9f * 1.0001f;

float readF32(const uint8_t *p) {
  float f;
  std::memcpy(&f, p, sizeof(f));
  return f;
}

uint16_t readU16(const uint8_t *p) {
  return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8));
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size < kHeaderBytes) {
    return 0;
  }

  HeaterControl::Config cfg;
  cfg.gains.kp = readF32(data + 0);
  cfg.gains.ki = readF32(data + 4);
  cfg.gains.kd = readF32(data + 8);
  cfg.dMeasTauS = readF32(data + 12);
  cfg.dutyMin = 0.0f;
  const float rawMax = readF32(data + 16);
  cfg.dutyMax = (std::isfinite(rawMax) && rawMax > 0.0f && rawMax <= 1.0f) ? rawMax : 1.0f;

  FakeClock clk;
  HeaterControl pi(clk, cfg);

  const size_t ticks = (size - kHeaderBytes) / kTickRecord;
  for (size_t i = 0; i < ticks; ++i) {
    const uint8_t *rec = data + kHeaderBytes + i * kTickRecord;
    const float setpointC = readF32(rec + 0);
    const float measuredC = readF32(rec + 4);
    const float ffDuty = readF32(rec + 8);
    const uint16_t step = readU16(rec + 12);
    const bool doReset = (rec[14] & 0x01) != 0;

    if (doReset) {
      pi.reset();
      FUZZ_ASSERT(pi.integrator() == 0.0f); // reset clears the integrator...
      FUZZ_ASSERT(pi.duty() == 0.0f);       // ...and the duty
    }

    clk.advance(step);
    const float duty = pi.update(setpointC, measuredC, ffDuty);

    // Output-safety invariant: duty is finite and within the configured range, always.
    FUZZ_ASSERT(std::isfinite(duty));
    FUZZ_ASSERT(duty >= cfg.dutyMin - 1.0e-6f);
    FUZZ_ASSERT(duty <= cfg.dutyMax + 1.0e-6f);
    FUZZ_ASSERT(pi.duty() == duty); // the accessor agrees with the return value

    // The integrator never blows up — anti-windup holds under any gains/inputs.
    FUZZ_ASSERT(std::isfinite(pi.integrator()));
    FUZZ_ASSERT(std::fabs(pi.integrator()) <= kIntegratorBound);

    // Blind control (non-finite setpoint or measurement) fails safe to OFF.
    if (!std::isfinite(setpointC) || !std::isfinite(measuredC)) {
      FUZZ_ASSERT(duty == 0.0f);
    }
  }
  return 0;
}
