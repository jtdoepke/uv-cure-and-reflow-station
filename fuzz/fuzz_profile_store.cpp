// Untrusted-input harness: control::ProfileStore (lib/control_logic/profile_library.h), the profile
// library's on-flash deserializer.
//
// RETARGETED from the CYD's old lib/app_logic/profile_store.h, which Wave R3 deleted. Wave R2 moved
// this store onto the SAFETY MCU and changed its layout — a small versioned header wrapping a
// nanopb-encoded oven_Profile ("PRO2"), not the CYD's old memcpy struct — so the harness was
// fuzzing a store no board runs while the one that parses attacker-reachable bytes on the safety
// MCU had no dedicated coverage.
//
// Two untrusted sources reach load(), and this covers the one nothing else did:
//   - a wire ProfilePut (§9). Already exercised end-to-end by fuzz_frontdoor/fuzz_decode, which
//     drive raw bytes through TinyFrame → router → ManagementResponder → this store.
//   - a raw FLASH blob. Nothing tests it, and it is genuinely reachable: §7 lets a profile be
//     pushed onto the device without a reflash, LittleFS is mounted formatOnFail so a half-written
//     blob survives a reset, and the controller decodes whatever the filesystem hands back.
//
// Three properties:
//   1. Robustness — the whole fuzz input is stored as a raw blob and driven through list()+load().
//      ASAN/UBSAN catch any OOB/UB; FUZZ_ASSERT that a *successful* load yields a well-formed
//      oven_Profile (phases_count <= kMaxPhases, name NUL-terminated, mode == the store's mode —
//      §7's "never mixed" guard enforced at the store, not just by the directory). A malformed blob
//      must be rejected, never mis-parsed.
//   2. Round-trip — a structured profile carved from the same bytes is save()'d then load()'d and
//      asserted faithful, so the success path is exercised and not only the reject path.
//   3. Differential — a successfully-loaded profile is decoded through the real phase_codec and fed
//      to the real compileRecipe(), which is exactly the path a fetched Profile takes on the CYD
//      (R3). A profile that survives deserialization is always a valid compiler input; a store that
//      green-lights a blob its own downstream chokes on is the bug — the contract fuzz_compiler
//      pins on the producer side. Spanning the wire types now, since the store speaks them.
//
// Input format (documented so seed_gen.cpp can emit an on-contract seed; little-endian floats):
//   [0]      flags : bit0 → mode (0 = Reflow, 1 = Cure); bit1 → stock
//   [1]      requested phase count (0..255; clamped to the records available and, for the array
//            fill, to kMaxPhases — a raw value past kMaxPhases exercises save()'s bound guard)
//   [2]      name length (1..kNameCharsMax; clamped to the bytes available)
//   [3..]    name-length bytes, each mapped onto a filesystem-safe charset (always a valid name)
//   then N × 17-byte phase records (same layout as fuzz_compiler.cpp):
//     [0..3] targetC [4..7] rampSeconds [8..11] holdSeconds [12..15] exposurePerSurface
//     [16]   flags: bit0 uv, bit1 motor, bits2-3 convFan{0,1,2} (bits4-5 reserved — no cool fan §6)
#include <cmath>
#include <cstring>
#include <vector>

#include "fuzz_util.h"

#include "helpers/fake_profile_storage.h"
#include "oven_cal.h"
#include "oven_safety.h"
#include "phase_codec.h"
#include "profile_library.h"
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

oven_FanMode readFan(uint8_t bits) {
  return static_cast<oven_FanMode>(bits > 2 ? 0 : bits);
}

// Validate the shape of any profile the store hands back — the invariant a rejection must never let
// slip. Shared by all three properties.
void assertWellFormed(const control::ProfileStore &store, const oven_Profile &p) {
  FUZZ_ASSERT(p.phases_count <= control::ProfileStore::kMaxPhases);
  // nanopb NUL-terminates every decoded string field; assert it rather than assume it, since every
  // downstream strcmp/strcpy on this name depends on it.
  FUZZ_ASSERT(std::memchr(p.name, '\0', sizeof(p.name)) != nullptr);
  for (pb_size_t i = 0; i < p.phases_count; ++i) {
    FUZZ_ASSERT(std::memchr(p.phases[i].name, '\0', sizeof(p.phases[i].name)) != nullptr);
  }
  // §7 "never mixed", enforced at the store: a cure blob landing in the reflow directory (a
  // cross-mode push, or a filesystem that got shuffled) is ignored, not merely kept apart by the
  // directory it sits in.
  FUZZ_ASSERT(p.mode == store.mode());
}

// The differential: run a loaded profile down the CYD's real consumption path (decode to the
// authored Phase[] via phase_codec, then compile). No UB, and any hard-valid compile keeps the
// structural promises B1 makes.
void assertCompilable(const oven_Profile &p) {
  Phase phases[kMaxPhases];
  const size_t n = phase_codec::phasesFromWire(p, phases, kMaxPhases);
  const RecipeMode mode = phase_codec::modeFromWire(p.mode);
  const Caps caps{oven_safety::MIN_SEGMENT_C, mode == RecipeMode::Cure
                                                  ? oven_safety::CURE_HARD_MAX_C
                                                  : oven_safety::REFLOW_HARD_MAX_C};
  const CompileResult r = compileRecipe(phases, n, mode, oven_cal::kDefaultModel, caps,
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

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size < kHeaderBytes) {
    return 0;
  }

  const oven_Mode mode = (data[0] & 1) ? oven_Mode_MODE_CURE : oven_Mode_MODE_REFLOW;
  const bool stock = (data[0] & 2) != 0;

  // --- Property 1: raw bytes straight into the deserializer ---
  {
    FakeProfileStorage fs;
    fs.put("raw", std::vector<uint8_t>(data, data + size));
    control::ProfileStore store(fs, mode);

    control::ProfileStore::Summary rows[control::ProfileStore::kMaxListed];
    store.list(rows, control::ProfileStore::kMaxListed); // must not UB on an arbitrary blob

    oven_Profile out = oven_Profile_init_zero;
    if (store.load("raw", out)) {
      assertWellFormed(store, out);
      assertCompilable(out);
    }
  }

  // --- Properties 2+3: carve a structured profile, round-trip it, then compile it ---
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

  oven_Profile p = oven_Profile_init_zero;
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
  const size_t fillClamped =
      fill < control::ProfileStore::kMaxPhases ? fill : control::ProfileStore::kMaxPhases;
  for (size_t i = 0; i < fillClamped; ++i) {
    const uint8_t *rec = data + recordBase + i * kRecordBytes;
    p.phases[i].target_c = readF32(rec + 0);
    p.phases[i].ramp_s = readF32(rec + 4);
    p.phases[i].hold_s = readF32(rec + 8);
    p.phases[i].exposure_per_surface = readF32(rec + 12);
    const uint8_t flags = rec[16];
    p.phases[i].uv = (flags & 0x01) != 0;
    p.phases[i].motor = (flags & 0x02) != 0;
    p.phases[i].fan_mode = readFan((flags >> 2) & 0x03);
    // bits4-5 reserved (was coolFan — no chamber cool fan, §6)
  }
  // Report the *requested* count, which may exceed kMaxPhases — that is the point, since save()'s
  // bound guard is one of the branches under test. Safe because the guard returns before anything
  // reads p.phases[] or hands it to the encoder, so an over-count never indexes past the fill.
  p.phases_count = static_cast<pb_size_t>(reqCount);

  FakeProfileStorage fs;
  control::ProfileStore store(fs, mode);
  if (store.save(p)) {
    // A saved profile must reload identically (bar the fields the store owns authoritatively: it
    // stamps mode from its own directory and use_seq from its recency counter, §23).
    oven_Profile out = oven_Profile_init_zero;
    FUZZ_ASSERT(store.load(p.name, out));
    assertWellFormed(store, out);
    FUZZ_ASSERT(out.phases_count == p.phases_count);
    FUZZ_ASSERT(std::strcmp(out.name, p.name) == 0);
    FUZZ_ASSERT(out.stock == p.stock);
    assertCompilable(out);
  }

  return 0;
}
