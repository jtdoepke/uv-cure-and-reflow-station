// Internal-correctness / differential harness: the §15 cure resume generator (B6,
// lib/app_logic/remainder.h).
//
// Why this needs fuzzing rather than just its unit tests: the remainder is a profile the operator
// never authored and never reviews. Every other path to a run goes through the editor's validation
// and the Confirm screen's preview; this one is generated from an interrupted run and handed
// straight to Start. So it inherits fuzz_compiler's core property and raises the stakes —
//
//   a remainder built from a HARD-VALID profile must ITSELF be hard-valid,
//
// because a remainder that fails to compile is a cure the operator cannot finish, and one that
// compiles but the controller NAKs is worse. The input profile is adversarial (the same raw
// Phase[] shape fuzz_profile_facts/fuzz_run_tracker use), so this also covers the untrusted-blob
// path: a profile loaded off the wire can be run, interrupted, and resumed.
//
// Plus the invariants the generator itself owes §15:
//   - it never grows a profile (the remainder is a suffix, so phaseCount <= the original's);
//   - the first phase is always an ASAP re-heat (rampSeconds == 0);
//   - hold/dose only ever SHRINK — resuming can never ask for more soak than was authored;
//   - reflow never produces one at all.
//
// Input format mirrors fuzz_profile_facts.cpp (header + 17-byte phase records), with the header
// carrying the interrupted phase index and the delivered fraction.
#include <cmath>
#include <cstring>

#include "fuzz_util.h"

#include "oven_cal.h"
#include "phase.h"
#include "profile_draft.h"
#include "recipe_compiler.h"
#include "oven_safety.h"      // the caps the real call site derives (see below)
#include "recipe_validator.h" // the real controller backstop, as in fuzz_compiler
#include "remainder.h"

namespace {

constexpr size_t kHeaderBytes = 8;
constexpr size_t kRecordBytes = 17;

float readF32(const uint8_t *p) {
  float f;
  std::memcpy(&f, p, sizeof(f));
  return f;
}

FanMode readFan(uint8_t bits) {
  return static_cast<FanMode>(bits > 2 ? 0 : bits);
}

// Bitwise float equality: `==` is useless here because the adversarial profiles are full of NaNs
// and NaN != NaN, which would make "carried through untouched" vacuously false.
bool sameBits(float a, float b) {
  uint32_t x = 0;
  uint32_t y = 0;
  std::memcpy(&x, &a, sizeof(x));
  std::memcpy(&y, &b, sizeof(y));
  return x == y;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size < kHeaderBytes) {
    return 0;
  }

  // Both modes are driven: reflow must be REFUSED, which is as much a property as the cure path.
  const RecipeMode mode = (data[0] & 1) ? RecipeMode::Cure : RecipeMode::Reflow;
  const OvenModel &model = oven_cal::kDefaultModel;

  const size_t avail = (size - kHeaderBytes) / kRecordBytes;
  size_t n = data[1];
  if (n > avail) {
    n = avail;
  }
  if (n > kMaxPhases) {
    n = kMaxPhases;
  }

  ProfileDraft src{};
  src.mode = mode;
  src.phaseCount = n;
  std::memcpy(src.name, "fz", 3);
  for (size_t i = 0; i < n; ++i) {
    const uint8_t *rec = data + kHeaderBytes + i * kRecordBytes;
    src.phases[i].targetC = readF32(rec + 0);
    src.phases[i].rampSeconds = readF32(rec + 4);
    src.phases[i].holdSeconds = readF32(rec + 8);
    src.phases[i].exposurePerSurface = readF32(rec + 12);
    const uint8_t flags = rec[16];
    src.phases[i].uv = flags & 0x01;
    src.phases[i].motor = flags & 0x02;
    src.phases[i].convFan = readFan((flags >> 2) & 0x03);
    std::memcpy(src.phases[i].name, "p", 2);
  }

  const size_t phaseIndex = data[2];         // deliberately often out of range
  const float delivered = readF32(data + 3); // raw float: NaN/Inf/huge/negative all reachable

  ProfileDraft out{};
  const bool built = cure_resume::build(src, phaseIndex, delivered, out);

  if (!built) {
    return 0; // refusing is always a valid answer; nothing further to check
  }
  FUZZ_ASSERT(mode == RecipeMode::Cure); // §15: reflow must never produce one

  // Structural invariants: a remainder is a SUFFIX of the original with its head rewritten.
  FUZZ_ASSERT(out.phaseCount > 0);
  FUZZ_ASSERT(out.phaseCount <= src.phaseCount);
  FUZZ_ASSERT(out.phaseCount <= kMaxPhases);
  FUZZ_ASSERT(out.mode == RecipeMode::Cure);
  FUZZ_ASSERT(!out.stock);
  FUZZ_ASSERT(out.phases[0].rampSeconds == 0.0F); // §15's ASAP re-heat, always

  // The suffix maps back onto the original: whichever phase the remainder starts at, its tail must
  // be the original's tail verbatim. Locating it by length is exact — a suffix of length k starts
  // at phaseCount - k.
  //
  // FIELD-BY-FIELD, never memcmp. `Phase` is 35 bytes of members in a 36-byte struct, and
  // copy-assignment is defined as member-wise — it never writes the pad — so the padding byte holds
  // whatever was last on the stack. The first version of this assertion used memcmp and the fuzzer
  // duly reported "byte 35: 07 vs 00", which was the harness reading uninitialised memory rather
  // than the generator losing data.
  const size_t first = src.phaseCount - out.phaseCount;
  for (size_t i = 1; i < out.phaseCount; ++i) {
    const Phase &a = out.phases[i];
    const Phase &b = src.phases[first + i];
    FUZZ_ASSERT(std::strncmp(a.name, b.name, kPhaseNameCap) == 0);
    FUZZ_ASSERT(sameBits(a.targetC, b.targetC));         // bitwise: NaN == NaN is false, and an
    FUZZ_ASSERT(sameBits(a.rampSeconds, b.rampSeconds)); // adversarial profile is full of NaNs
    FUZZ_ASSERT(sameBits(a.holdSeconds, b.holdSeconds));
    FUZZ_ASSERT(sameBits(a.exposurePerSurface, b.exposurePerSurface));
    FUZZ_ASSERT(a.uv == b.uv && a.motor == b.motor && a.convFan == b.convFan);
  }
  // The head keeps its identity (target/channels) but never asks for MORE soak than was authored.
  // NaN-safe: a NaN authored hold makes both sides NaN and the comparison is skipped, which is the
  // compiler's problem to reject, not this one's.
  const Phase &head = out.phases[0];
  const Phase &orig = src.phases[first];
  FUZZ_ASSERT(!(head.targetC == head.targetC) || head.targetC == orig.targetC);
  FUZZ_ASSERT(head.uv == orig.uv && head.motor == orig.motor);
  if (std::isfinite(orig.holdSeconds) && std::isfinite(head.holdSeconds)) {
    FUZZ_ASSERT(std::fabs(head.holdSeconds) <= std::fabs(orig.holdSeconds) + 1e-3F);
  }
  if (std::isfinite(orig.exposurePerSurface) && std::isfinite(head.exposurePerSurface)) {
    FUZZ_ASSERT(std::fabs(head.exposurePerSurface) <= std::fabs(orig.exposurePerSurface) + 1e-3F);
  }

  // THE DIFFERENTIAL: whatever the remainder compiles to, the real controller backstop must accept
  // it — the same one-directional property fuzz_compiler holds for authored profiles, extended to
  // the one profile nobody authored. THIS is the safety-relevant claim, because a remainder that
  // compiles but NAKs on upload strands the operator mid-cure with the door already open.
  //
  // NOTE what is deliberately NOT asserted: "the original compiles ⇒ the remainder compiles". That
  // is false, and the fuzzer proved it — a profile sitting at the 32-segment wire ceiling can tip
  // over when the head is rewritten, because forcing rampSeconds to 0 switches that phase from a
  // timed ramp to RAMP_ASAP and B1 lowers the two differently. (The witness had an authored ramp of
  // 3.7e-40 s: non-zero, so timed; the remainder's ASAP head pushed a 32-segment recipe to
  // 0/invalid.) Unreachable with realistic phase counts, but it is a real property of the
  // generator, and the right answer is not to weaken the generator — it is for the CALLER to gate
  // Resume on the remainder compiling, exactly as Confirm gates Start for every other run. See
  // run_screen. Caps exactly as the real call site derives them (fuzz_compiler says the same): a
  // user cap can never loosen past the controller's, and CURE is the LOWER ceiling because any
  // uv/motor segment forces it (§4). Compiling against a looser cap than the controller enforces
  // was the first version of this line, and the fuzzer immediately produced a hard-valid compile
  // the validator then NAKed — the harness's bug, not the generator's, but the exact shape of the
  // bug this differential exists to catch.
  const Caps caps{oven_safety::MIN_SEGMENT_C, oven_safety::CURE_HARD_MAX_C};
  const CompileResult b =
      compileRecipe(out.phases, out.phaseCount, RecipeMode::Cure, model, caps, 25.0F, 0, 0);
  if (b.hardValid) {
    RecipeValidator validator;
    oven_NakReason reason = oven_NakReason_NAK_UNSPECIFIED;
    FUZZ_ASSERT(validator.validateRecipe(b.recipe, reason));
  }
  return 0;
}
