// Internal-property harness: the CYD's recipe compiler (B1, lib/app_logic/recipe_compiler.h)
// must never emit a recipe the controller's own backstop rejects.
//
// This is the *producer* side of the untrusted-input pipeline: RecipeValidator (fuzzed directly
// by fuzz_validator) is what the controller runs on whatever crosses the wire; the compiler is
// what the CYD runs to build that wire recipe from operator-authored phases. Their contract is a
// one-directional differential — **every hard-valid compile is accepted by the real validator** —
// so a compiler that green-lights a recipe the controller would NAK is the bug this hunts. Unlike
// the unit test's hand-reproduced invariants, this links the actual RecipeValidator and lets
// coverage push at the cap/rounding/mode boundaries with adversarial floats (NaN/Inf/huge).
//
// Input format (documented so seed_gen.cpp can emit an on-contract seed; little-endian floats):
//   [0]      mode  : bit0 → 0 = Reflow, 1 = Cure
//   [1]      model : bit0 → 0 = uncalibrated (oven_cal::kDefaultModel), 1 = a calibrated preset
//   [2..5]   ambientC : float32
//   [6]      requested phase count (clamped to the bytes available and to kFuzzMaxPhases)
//   then N × 17-byte phase records:
//     [0..3] targetC  [4..7] rampSeconds  [8..11] holdSeconds  [12..15] exposurePerSurface
//     [16]   flags: bit0 uv, bit1 motor, bits2-3 convFan{0,1,2}, bits4-5 coolFan{0,1,2}
#include <cmath>
#include <cstring>

#include "fuzz_util.h"

#include "oven_cal.h"
#include "recipe_compiler.h"
#include "recipe_validator.h" // the real controller backstop (pulls oven_safety.h)

namespace {

constexpr size_t kHeaderBytes = 7;
constexpr size_t kRecordBytes = 17;
// Above the 32-segment budget so the TooManySegments path is one mutation away.
constexpr size_t kFuzzMaxPhases = 40;

float readF32(const uint8_t *p) {
  float f;
  std::memcpy(&f, p, sizeof(f)); // raw bytes → any float, incl. NaN/Inf/denormal
  return f;
}

FanMode readFan(uint8_t bits) {
  return static_cast<FanMode>(bits > 2 ? 0 : bits);
}

RateEnvelope constRate(float r) {
  return RateEnvelope{0.0f, r, 0.01f, r};
}

// A contract-valid calibrated plant (floors > 0), so the harness fuzzes the compiler over a legal
// model rather than an illegal one — the compiler's job is not to defend against a broken oven_cal.
OvenModel calibratedPreset() {
  OvenModel m{};
  m.heat = FanPair<RateEnvelope>{constRate(1.0f), constRate(2.0f)};
  m.cool = FanPair<RateEnvelope>{constRate(0.5f), constRate(1.0f)};
  m.lag = FanPair<LagParams>{LagParams{1.0f, 0.0f, 10.0f}, LagParams{1.0f, 0.0f, 10.0f}};
  m.duty = FanPair<DutyModel>{DutyModel{0.0f, 0.5f, 1.0f}, DutyModel{0.0f, 0.5f, 1.0f}};
  m.beamCoverage = 0.5f;
  m.turntableRpm = 30.0f;
  m.calibrated = true;
  return m;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size < kHeaderBytes) {
    return 0;
  }

  const RecipeMode mode = (data[0] & 1) ? RecipeMode::Cure : RecipeMode::Reflow;
  const OvenModel &model = (data[1] & 1) ? calibratedPreset() : oven_cal::kDefaultModel;
  const float ambientC = readF32(data + 2);

  const size_t available = (size - kHeaderBytes) / kRecordBytes;
  size_t n = data[6];
  if (n > available) {
    n = available;
  }
  if (n > kFuzzMaxPhases) {
    n = kFuzzMaxPhases;
  }

  Phase phases[kFuzzMaxPhases];
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
    phases[i].coolFan = readFan((flags >> 4) & 0x03);
  }

  // Caps as the real call site derives them: a user cap can never loosen past the controller's
  // reviewed per-mode hard-max (SettingsStore clamps it at boot), so validating the compile against
  // that hard-max is exactly the airtight side of the differential — anything B1 accepts here the
  // controller must too. (A tighter user cap only makes B1 stricter, never the reverse.)
  const Caps caps{oven_safety::MIN_SEGMENT_C, mode == RecipeMode::Cure
                                                  ? oven_safety::CURE_HARD_MAX_C
                                                  : oven_safety::REFLOW_HARD_MAX_C};

  CompileResult r = compileRecipe(phases, n, mode, model, caps, ambientC, /*id=*/1, /*seq=*/1);

  if (!r.hardValid) {
    // A rejected compile must carry no recipe (the fail path zeroes it).
    FUZZ_ASSERT(r.recipe.segments_count == 0);
    return 0;
  }

  // Structural invariants B1 promises about a hard-valid recipe.
  FUZZ_ASSERT(r.recipe.segments_count >= 1);
  FUZZ_ASSERT(r.recipe.segments_count <= kMaxSegments);
  for (pb_size_t i = 0; i < r.recipe.segments_count; ++i) {
    FUZZ_ASSERT(r.recipe.segments[i].dur_ms > 0);
    FUZZ_ASSERT(std::isfinite(r.recipe.segments[i].heat_c));
  }

  // The differential: the actual controller backstop must accept what the compiler blessed.
  RecipeValidator validator;
  oven_NakReason reason = oven_NakReason_NAK_UNSPECIFIED;
  FUZZ_ASSERT(validator.validateRecipe(r.recipe, reason));
  return 0;
}
