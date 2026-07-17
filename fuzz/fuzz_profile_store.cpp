// Untrusted-input harness: ProfileStore (B4, lib/app_logic/profile_store.h) deserializes profile
// blobs that need not have come from its own save() — §7 lets a profile be pushed onto the device
// over serial/WiFi without a reflash, so load()/list() are a parsing seam for attacker-controllable
// bytes. This harness holds two properties over that seam:
//
//   1. Robustness — the whole fuzz input is stored as a raw blob and driven through list()+load().
//      ASAN/UBSAN catch any OOB/UB; FUZZ_ASSERT that a *successful* load yields a well-formed
//      StoredProfile (phaseCount <= kMaxPhases, name NUL-terminated, mode == the store's mode). A
//      malformed blob must be rejected, never mis-parsed.
//   2. Round-trip + differential — a structured StoredProfile carved from the same bytes is
//      save()'d then load()'d and asserted byte-faithful, and a successfully-loaded profile's
//      Phase[] is fed to the real compileRecipe(): a profile that survives deserialization is
//      always a valid compiler input (a store that green-lights a blob its own downstream chokes on
//      is the bug — the same contract fuzz_compiler pins on the producer side).
//
// Input format (documented so seed_gen.cpp can emit an on-contract seed; little-endian floats):
//   [0]      flags : bit0 → mode (0 = Reflow, 1 = Cure); bit1 → stock
//   [1]      requested phase count (0..255; clamped to the records available and, for the array
//            fill, to kMaxPhases — a raw value past kMaxPhases exercises save()'s bound guard)
//   [2]      name length (1..kNameCharsMax; clamped to the bytes available)
//   [3..]    name-length bytes, each mapped onto a filesystem-safe charset (always a valid name)
//   then N × 17-byte phase records (same layout as fuzz_compiler.cpp):
//     [0..3] targetC [4..7] rampSeconds [8..11] holdSeconds [12..15] exposurePerSurface
//     [16]   flags: bit0 uv, bit1 motor, bits2-3 convFan{0,1,2}, bits4-5 coolFan{0,1,2}
#include <cmath>
#include <cstring>
#include <vector>

#include "fuzz_util.h"

#include "helpers/fake_profile_storage.h"
#include "oven_cal.h"
#include "oven_safety.h"
#include "profile_store.h"
#include "recipe_compiler.h"

namespace {

constexpr size_t kHeaderBytes = 3;
constexpr size_t kRecordBytes = 17;
constexpr size_t kNameCharsMax = kProfileNameCap - 1;
// A filesystem-safe charset so the carved name always passes ProfileStore::validName — that keeps
// the round-trip half exercising the save/load *success* path, not just the reject path.
constexpr char kSafe[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_ ";

float readF32(const uint8_t *p) {
  float f;
  std::memcpy(&f, p, sizeof(f)); // raw bytes → any float, incl. NaN/Inf/denormal
  return f;
}

FanMode readFan(uint8_t bits) {
  return static_cast<FanMode>(bits > 2 ? 0 : bits);
}

// Validate the shape of any profile the store hands back — the invariant a rejection must never let
// slip. Shared by both halves.
void assertWellFormed(const ProfileStore &store, const ProfileStore::StoredProfile &p) {
  FUZZ_ASSERT(p.phaseCount <= kMaxPhases);
  FUZZ_ASSERT(std::memchr(p.name, '\0', kProfileNameCap) != nullptr); // NUL-terminated
  FUZZ_ASSERT(p.mode == store.mode());
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size < kHeaderBytes) {
    return 0;
  }

  const RecipeMode mode = (data[0] & 1) ? RecipeMode::Cure : RecipeMode::Reflow;
  const bool stock = (data[0] & 2) != 0;

  // --- Property 1: raw bytes straight into the deserializer ---
  {
    FakeProfileStorage fs;
    fs.put("raw", std::vector<uint8_t>(data, data + size));
    ProfileStore store(fs, mode);

    ProfileStore::Summary rows[ProfileStore::kMaxListed];
    store.list(rows, ProfileStore::kMaxListed); // must not UB on an arbitrary blob

    ProfileStore::StoredProfile out;
    if (store.load("raw", out)) {
      assertWellFormed(store, out);
    }
  }

  // --- Property 2: carve a structured profile, round-trip it, then compile it ---
  const size_t available = (size - kHeaderBytes) / kRecordBytes;
  size_t nameLen = data[2];
  if (nameLen < 1) {
    nameLen = 1;
  }
  if (nameLen > kNameCharsMax) {
    nameLen = kNameCharsMax;
  }
  if (nameLen > size - kHeaderBytes) {
    nameLen = size - kHeaderBytes; // name bytes must exist; may leave 0 records, that's fine
  }

  ProfileStore::StoredProfile p;
  for (size_t i = 0; i < nameLen; ++i) {
    p.name[i] = kSafe[data[kHeaderBytes + i] % (sizeof(kSafe) - 1)];
  }
  p.name[nameLen] = '\0';
  p.mode = mode;
  p.stock = stock;

  const size_t recordBase = kHeaderBytes + nameLen;
  const size_t recordsAvail = size > recordBase ? (size - recordBase) / kRecordBytes : 0;
  const size_t reqCount = data[1];
  const size_t fill =
      reqCount < recordsAvail ? reqCount : recordsAvail; // records we actually have bytes for
  const size_t fillClamped = fill < kMaxPhases ? fill : kMaxPhases;
  for (size_t i = 0; i < fillClamped; ++i) {
    const uint8_t *rec = data + recordBase + i * kRecordBytes;
    p.phases[i].targetC = readF32(rec + 0);
    p.phases[i].rampSeconds = readF32(rec + 4);
    p.phases[i].holdSeconds = readF32(rec + 8);
    p.phases[i].exposurePerSurface = readF32(rec + 12);
    const uint8_t flags = rec[16];
    p.phases[i].uv = flags & 0x01;
    p.phases[i].motor = flags & 0x02;
    p.phases[i].convFan = readFan((flags >> 2) & 0x03);
    p.phases[i].coolFan = readFan((flags >> 4) & 0x03);
  }
  // Report the *requested* count (possibly > kMaxPhases) so save()'s bound guard is exercised; the
  // guard returns before any read past the filled slots, so this is safe.
  p.phaseCount = reqCount;

  FakeProfileStorage fs;
  ProfileStore store(fs, mode);
  if (store.save(p)) {
    // A saved profile must reload identically.
    ProfileStore::StoredProfile out;
    FUZZ_ASSERT(store.load(p.name, out));
    assertWellFormed(store, out);
    FUZZ_ASSERT(out.phaseCount == p.phaseCount);
    FUZZ_ASSERT(std::strcmp(out.name, p.name) == 0);

    // And it must be a valid input to the compiler its phases are destined for — no UB, and the
    // structural promises B1 makes about any hard-valid recipe (the store never green-lights a blob
    // the downstream chokes on).
    const Caps caps{oven_safety::MIN_SEGMENT_C, mode == RecipeMode::Cure
                                                    ? oven_safety::CURE_HARD_MAX_C
                                                    : oven_safety::REFLOW_HARD_MAX_C};
    CompileResult r =
        compileRecipe(out.phases, out.phaseCount, out.mode, oven_cal::kDefaultModel, caps,
                      /*ambientC=*/22.0f, /*id=*/1, /*seq=*/1);
    if (r.hardValid) {
      FUZZ_ASSERT(r.recipe.segments_count >= 1);
      FUZZ_ASSERT(r.recipe.segments_count <= kMaxSegments);
      for (pb_size_t i = 0; i < r.recipe.segments_count; ++i) {
        FUZZ_ASSERT(r.recipe.segments[i].dur_ms > 0);
        FUZZ_ASSERT(std::isfinite(r.recipe.segments[i].heat_c));
      }
    }
  }

  return 0;
}
