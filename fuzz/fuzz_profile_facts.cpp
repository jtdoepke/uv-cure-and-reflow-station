// Internal-correctness harness: profile_facts (C4, lib/app_logic/profile_facts.h) is a new consumer
// of an untrusted Phase[]. A profile can be pushed onto the device over serial/WiFi (§7), and
// ProfileStore's deserializer bounds phaseCount + the name but NOT the Phase float fields — so
// targetC/rampSeconds/holdSeconds/exposurePerSurface reach computeFacts()/sampleCurve() as
// NaN/Inf/huge. This harness drives a raw Phase[] (adversarial floats, both modes, an adversarial
// ambient, calibrated + uncalibrated models) and holds the invariants the LVGL curve widget and the
// formatters rely on — so the widget can stay dumb (it only scales already-bounded points):
//
//   - computeFacts(): peakC finite in [kTempLo,kTempHi]; totalSeconds finite in [0,kMaxSeconds];
//     phaseCount <= kMaxPhases.
//   - sampleCurve() (requested AND achievable): <= kMaxCurvePoints points, each finite and within
//     the same declared bounds — no NaN coordinate can ever reach lv_line.
//   - sampleOvershoot() (C5 closed-loop preview): an ITERATIVE {a,b,τ} lag over the achievable
//     trajectory — the new NaN/divergence surface (cf. fuzz_heater_control on the PID). Bounded to
//     kMaxCurvePoints points, each finite + in-bounds, time monotonic, no matter how adversarial
//     the Phase[] / ambient / model — a NaN setpoint must not poison the running lag state.
//   - anyRampRateLimited() (C5 divergence flag): total, no UB, on any input.
//   - formatDuration()/formatPeak(): always NUL-terminate their buffer (no overflow) on any input.
//
// ASAN/UBSAN catch any OOB/UB in the maths itself. Joins the fuzz_profile_store / fuzz_compiler
// family (fuzz/README.md).
//
// Input format (little-endian floats):
//   [0]      flags : bit0 → mode (0 = Reflow, 1 = Cure); bit1 → calibrated model; bit2 → Fahrenheit
//   [1]      requested phase count (0..255; clamped to the records available and to kMaxPhases)
//   [2..5]   ambient °C (raw float — incl. NaN/Inf, exercising clampT on the start temp)
//   then N × 17-byte phase records (same layout as fuzz_compiler.cpp / fuzz_profile_store.cpp):
//     [0..3] targetC [4..7] rampSeconds [8..11] holdSeconds [12..15] exposurePerSurface
//     [16]   flags: bit0 uv, bit1 motor, bits2-3 convFan{0,1,2} (bits4-5 reserved — no cool fan §6)
#include <cmath>
#include <cstring>

#include "fuzz_util.h"

#include "oven_cal.h"
#include "profile_facts.h"

namespace {

constexpr size_t kHeaderBytes = 6;
constexpr size_t kRecordBytes = 17;

float readF32(const uint8_t *p) {
  float f;
  std::memcpy(&f, p, sizeof(f)); // raw bytes → any float, incl. NaN/Inf/denormal
  return f;
}

FanMode readFan(uint8_t bits) {
  return static_cast<FanMode>(bits > 2 ? 0 : bits);
}

bool inBounds(float v, float lo, float hi) {
  return std::isfinite(v) && v >= lo && v <= hi;
}

// A toy affine-rate model, so the fuzzer also exercises the temperature-dependent envelope path
// (the stub oven_cal::kDefaultModel is constant-rate). Rates are floored > 0 like the real ones, so
// the integrand never divides by ~0.
OvenModel toyCalibrated() {
  RateEnvelope heat{0.004f, 0.6f, 0.05f, 2.0f};
  RateEnvelope cool{0.002f, 0.2f, 0.05f, 1.0f};
  LagParams lag{1.0f, 0.0f, 40.0f};
  DutyModel duty{0.002f, 0.05f, 1.0f};
  return OvenModel{{heat, heat}, {cool, cool}, {lag, lag}, {duty, duty}, 0.3f, 5.0f, true};
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size < kHeaderBytes) {
    return 0;
  }

  const RecipeMode mode = (data[0] & 1) ? RecipeMode::Cure : RecipeMode::Reflow;
  const OvenModel model = (data[0] & 2) ? toyCalibrated() : oven_cal::kDefaultModel;
  const bool fahrenheit = (data[0] & 4) != 0;
  const float ambient = readF32(data + 2); // adversarial start temp (NaN/Inf ok)

  const size_t avail = (size - kHeaderBytes) / kRecordBytes;
  size_t n = data[1];
  if (n > avail) {
    n = avail;
  }
  if (n > kMaxPhases) {
    n = kMaxPhases;
  }

  Phase phases[kMaxPhases];
  for (size_t i = 0; i < n; ++i) {
    const uint8_t *rec = data + kHeaderBytes + i * kRecordBytes;
    phases[i].targetC = readF32(rec + 0);
    phases[i].rampSeconds = readF32(rec + 4);
    phases[i].holdSeconds = readF32(rec + 8);
    phases[i].exposurePerSurface = readF32(rec + 12);
    const uint8_t flags = rec[16];
    phases[i].uv = flags & 0x01;
    phases[i].motor = flags & 0x02;
    phases[i].convFan = readFan((flags >> 2) & 0x03);
    // bits4-5 reserved (was coolFan — no chamber cool fan, §6)
  }

  // --- computeFacts: finite + bounded ---
  const profile_facts::ProfileFacts f =
      profile_facts::computeFacts(phases, n, mode, model, ambient);
  FUZZ_ASSERT(inBounds(f.peakC, profile_facts::kTempLo, profile_facts::kTempHi));
  FUZZ_ASSERT(inBounds(f.totalSeconds, 0.0f, profile_facts::kMaxSeconds));
  FUZZ_ASSERT(f.phaseCount <= kMaxPhases);

  // --- sampleCurve: every emitted point finite + within the declared box (both variants) ---
  profile_facts::CurvePoint pts[profile_facts::kMaxCurvePoints];
  for (int achievable = 0; achievable < 2; ++achievable) {
    const size_t m = profile_facts::sampleCurve(phases, n, mode, model, achievable != 0, ambient,
                                                pts, profile_facts::kMaxCurvePoints);
    FUZZ_ASSERT(m <= profile_facts::kMaxCurvePoints);
    for (size_t i = 0; i < m; ++i) {
      FUZZ_ASSERT(inBounds(pts[i].t, 0.0f, profile_facts::kMaxSeconds));
      FUZZ_ASSERT(inBounds(pts[i].T, profile_facts::kTempLo, profile_facts::kTempHi));
      if (i > 0) {
        FUZZ_ASSERT(pts[i].t >= pts[i - 1].t); // time is monotonic non-decreasing
      }
    }
  }

  // --- sampleOvershoot: bounded, finite, in-bounds, time monotonic (C5 iterative lag) ---
  profile_facts::CurvePoint over[profile_facts::kMaxCurvePoints];
  const size_t mo = profile_facts::sampleOvershoot(phases, n, mode, model, ambient, over,
                                                   profile_facts::kMaxCurvePoints);
  FUZZ_ASSERT(mo <= profile_facts::kMaxCurvePoints);
  for (size_t i = 0; i < mo; ++i) {
    FUZZ_ASSERT(inBounds(over[i].t, 0.0f, profile_facts::kMaxSeconds));
    FUZZ_ASSERT(inBounds(over[i].T, profile_facts::kTempLo, profile_facts::kTempHi));
    if (i > 0) {
      FUZZ_ASSERT(over[i].t >= over[i - 1].t);
    }
  }

  // --- anyRampRateLimited: total, no UB (the return value is unconstrained; UBSAN is the check)
  // ---
  (void)profile_facts::anyRampRateLimited(phases, n, mode, model, ambient);

  // --- samplePhaseBoundaries: bounded, finite, monotonic (the curve's phase-separator x's). The
  // effective list carries one extra boundary for the implicit cool-down phase (§6), so size to
  // kMaxCurvePhases. ---
  float bounds[profile_facts::kMaxCurvePhases];
  const size_t mb = profile_facts::samplePhaseBoundaries(phases, n, mode, model, ambient, bounds,
                                                         profile_facts::kMaxCurvePhases);
  FUZZ_ASSERT(mb <= profile_facts::kMaxCurvePhases);
  for (size_t i = 0; i < mb; ++i) {
    FUZZ_ASSERT(inBounds(bounds[i], 0.0f, profile_facts::kMaxSeconds));
    if (i > 0) {
      FUZZ_ASSERT(bounds[i] >= bounds[i - 1]);
    }
  }

  // --- sampleUvSpans: bounded, finite, ordered (start <= end) ---
  profile_facts::TimeSpan spans[kMaxPhases];
  const size_t ms =
      profile_facts::sampleUvSpans(phases, n, mode, model, ambient, spans, kMaxPhases);
  FUZZ_ASSERT(ms <= kMaxPhases);
  for (size_t i = 0; i < ms; ++i) {
    FUZZ_ASSERT(inBounds(spans[i].start, 0.0f, profile_facts::kMaxSeconds));
    FUZZ_ASSERT(inBounds(spans[i].end, 0.0f, profile_facts::kMaxSeconds));
    FUZZ_ASSERT(spans[i].end >= spans[i].start);
  }

  // --- formatters: never overrun their buffer, always NUL-terminated ---
  char buf[8];
  profile_facts::formatDuration(f.totalSeconds, buf, sizeof(buf));
  FUZZ_ASSERT(std::memchr(buf, '\0', sizeof(buf)) != nullptr);
  profile_facts::formatPeak(f.peakC, fahrenheit, buf, sizeof(buf));
  FUZZ_ASSERT(std::memchr(buf, '\0', sizeof(buf)) != nullptr);

  return 0;
}
