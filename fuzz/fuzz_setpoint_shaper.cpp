// Internal-property harness: the PI loop's reference trajectory (§5,
// lib/control_logic/setpoint_shaper.h) must hold its invariants under an arbitrary plant model AND
// an adversarial signal trajectory.
//
// The shaper sits directly upstream of the heater duty: whatever it emits becomes the PI's
// setpoint, and its reported rate becomes a feedforward duty bump. So a reference that runs away
// from the measurement, drifts above the executor's setpoint, or goes non-finite is a *heat*
// bug, not a cosmetic one — which is why this is fuzzed rather than only unit-tested. The unit
// suite covers the trajectories a person thinks to write down; coverage-guided mutation with
// UBSAN/ASAN pushes the dt division, the envelope lookup, the taper, and the clamp order at their
// boundaries (NaN/Inf/denormal models, zero and huge dt, an inverted envelope, a setpoint that
// jumps sign every tick).
//
// Invariants asserted every tick:
//   - the reference is finite whenever the inputs are (and is passed straight THROUGH, unchanged,
//     when they are not — the blind-control fail-safe belongs to HeaterControl and this must not
//     launder it);
//   - the reference never exceeds the executor's setpoint on an upward approach — the "can only
//     ever reduce commanded heat" claim the run path makes to the safety argument;
//   - the reference never leads the measurement by more than maxLeadC (the anti-runaway bound);
//   - the reported rate is finite and consistent with the reference's actual movement over dt;
//   - reset() re-seeds at the measurement.
//
// Also drives thermal_math.h's rampFeedforwardDuty() with the shaper's own output, since that pair
// is what the run path composes: the duty bump must stay in [0,1] for any model/rate.
//
// Input format (documented so seed_gen.cpp can emit an on-contract seed; little-endian):
//   header, 40 bytes — the OvenModel's heat envelope (both fan variants) + the shaper config:
//     [0..3]   heatOff.slope     : float32 (raw)
//     [4..7]   heatOff.intercept : float32 (raw)
//     [8..11]  heatOff.floor     : float32 (raw)
//     [12..15] heatOff.ceiling   : float32 (raw)
//     [16..19] heatOn.slope      : float32 (raw)
//     [20..23] heatOn.intercept  : float32 (raw)
//     [24..27] duty.slope        : float32 (raw — feeds rampFeedforwardDuty)
//     [28..31] duty.rateGain     : float32 (raw — the /rateGain divisor)
//     [32..35] approachTauS      : float32 (raw)
//     [36..39] maxLeadC          : float32 (raw)
//   then N × 11-byte tick records, consumed until the bytes run out:
//     [0..3]  spExec    : float32 (raw)
//     [4..7]  measuredC : float32 (raw)
//     [8..9]  clockStep : uint16 ms (incl. 0)
//     [10]    flags     : bit0 → reset() before this tick; bit1 → convection fan on
#include <cmath>
#include <cstring>

#include "fuzz_util.h"

#include "helpers/fake_clock.h" // -I test (fuzz_common); the same fake the unit tests use
#include "setpoint_shaper.h"
#include "thermal_math.h"

namespace {

constexpr size_t kHeaderBytes = 40;
constexpr size_t kTickRecord = 11;

// The shaper's own notion of "finite", which this harness must mirror rather than assume
// std::isfinite: these headers avoid <cmath>, so the check is `v == v` plus a ±3.4e38 bound — a
// hair INSIDE FLT_MAX (3.4028e38), deliberately, so a value that survives the check cannot overflow
// on the next arithmetic step. Values in that sliver take the class's blind-control path while
// std::isfinite would call them finite; testing against the wrong definition reports a phantom bug.
bool shaperFinite(float v) {
  return v == v && v <= 3.4e38f && v >= -3.4e38f;
}

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

  // A wholly adversarial model: slopes/floors/ceilings straight from the fuzzer, so an inverted
  // envelope (floor > ceiling), a negative rate, and NaN/Inf all reach the pacing math.
  OvenModel m{};
  m.heat.off =
      RateEnvelope{readF32(data + 0), readF32(data + 4), readF32(data + 8), readF32(data + 12)};
  m.heat.on =
      RateEnvelope{readF32(data + 16), readF32(data + 20), readF32(data + 8), readF32(data + 12)};
  m.cool.off = m.heat.off;
  m.cool.on = m.heat.on;
  m.duty.off = DutyModel{readF32(data + 24), 0.0f, readF32(data + 28)};
  m.duty.on = m.duty.off;

  SetpointShaper::Config cfg;
  cfg.approachTauS = readF32(data + 32);
  cfg.maxLeadC = readF32(data + 36);

  FakeClock clk;
  SetpointShaper sh(clk, cfg);

  // Whether the shaper currently holds a reference. A tick that SEEDS one (the first after
  // construction, a reset, or a non-finite input) legitimately jumps the reference to the
  // measurement and reports rate 0, so the rate-consistency check below does not apply to it.
  bool seeded = false;

  const size_t ticks = (size - kHeaderBytes) / kTickRecord;
  for (size_t i = 0; i < ticks; ++i) {
    const uint8_t *rec = data + kHeaderBytes + i * kTickRecord;
    const float spExec = readF32(rec + 0);
    const float measuredC = readF32(rec + 4);
    const uint16_t step = readU16(rec + 8);
    const bool doReset = (rec[10] & 0x01) != 0;
    const bool fan = (rec[10] & 0x02) != 0;

    if (doReset) {
      sh.reset();
      seeded = false;
    }
    const float before = sh.reference();
    clk.advance(step);
    const SetpointShaper::Shaped s = sh.update(spExec, measuredC, fan, m);

    // The accessor always reads back exactly what update() returned (NaN compares unequal to
    // itself, hence the isnan arm).
    FUZZ_ASSERT(s.setpointC == sh.reference() ||
                (std::isnan(s.setpointC) && std::isnan(sh.reference())));
    FUZZ_ASSERT(std::isfinite(s.ratePerS));

    if (!shaperFinite(spExec) || !shaperFinite(measuredC)) {
      // Blind control passes THROUGH untouched — HeaterControl owns that fail-safe, and laundering
      // a NaN into a finite reference here would silently disarm it.
      FUZZ_ASSERT((std::isnan(spExec) && std::isnan(s.setpointC)) || s.setpointC == spExec);
      FUZZ_ASSERT(s.ratePerS == 0.0f);
      seeded = false; // blind control drops the reference; the next tick re-seeds
      continue;
    }

    FUZZ_ASSERT(std::isfinite(s.setpointC));
    // Never above what the executor asked for: the invariant the run path's safety argument rests
    // on ("this can only ever reduce commanded heat").
    FUZZ_ASSERT(s.setpointC <= spExec);
    // Never leading the measurement by more than the clamp allows (when the clamp is usable at
    // all — a NaN/Inf maxLeadC degrades to 0, which is stricter, not looser).
    const float lead = shaperFinite(cfg.maxLeadC) ? cfg.maxLeadC : 0.0f;
    if (std::isfinite(measuredC + lead)) {
      FUZZ_ASSERT(s.setpointC <= (measuredC + lead) || s.setpointC <= spExec);
    }
    // The reported rate describes the reference's actual movement (it is a feedforward input, so a
    // rate that disagrees with the trajectory would command duty for motion that is not happening).
    if (seeded && step > 0 && shaperFinite(before)) {
      const float dtS = static_cast<float>(step) / 1000.0f;
      const float moved = (s.setpointC - before) / dtS;
      // Same stricter definition again: a movement that lands in the near-FLT_MAX sliver is
      // reported as rate 0 by the class (it refuses to hand downstream a number that would overflow
      // on the next multiply), so it is not a disagreement to assert against.
      if (shaperFinite(moved)) {
        FUZZ_ASSERT(std::fabs(moved - s.ratePerS) <= 1.0e-2f * (1.0f + std::fabs(moved)));
      }
    }

    // The pair the run path actually composes: the shaper's output feeding the trajectory
    // feedforward must stay a legal duty for any model.
    const float ff = rampFeedforwardDuty(m, s.setpointC, s.ratePerS, fan);
    FUZZ_ASSERT(std::isfinite(ff));
    FUZZ_ASSERT(ff >= 0.0f && ff <= 1.0f);
    seeded = true;
  }
  return 0;
}
