// Seam 3: decoded structs → the RecipeValidator backstop, checked directly.
//
// The input is carved into two nanopb payloads by a 1-byte length prefix (the format
// seed_gen.cpp mirrors): byte 0 = L, the next L bytes are decoded as a Recipe, the
// remainder as a Start — both on the same validator, so an accepted Recipe's id can
// satisfy the Start's "known recipe" check. The invariant is accept-implies-
// constraints: when the validator says yes, the message must actually satisfy every
// rule the backstop promises — a validator that accepts what it must reject is the
// bug this hunts.
//
// The validator's two Start guards (the A7 follow-up: NAK_ILLEGAL_TRANSITION and
// NAK_WORKPIECE_TC_INVALID) read a live ProfileExecutor and IThermocouples, so both are
// wired here to adversarial stand-ins driven off the SAME input bytes — a faulted or
// NaN-reading probe, an empty wall array, a run already executing. Without them those
// branches would never execute under the sanitizers, and the probe path in particular
// does float comparisons on wire-controlled values.
#include <cmath>
#include <cstring>

#include "fuzz_util.h"
#include "helpers/fake_clock.h"
#include "helpers/fake_thermocouples.h"
#include "profile_executor.h"
#include "recipe_validator.h" // pulls codec.h, oven.pb.h, oven_safety.h, workpiece_tc.h

namespace {

// A float from raw bits — deliberately including NaN/Inf, which is the whole point: the probe
// predicate compares wire-controlled floats, and every comparison against NaN is false.
float bitsToFloat(uint32_t bits) {
  float f = 0.0F;
  std::memcpy(&f, &bits, sizeof(f));
  return f;
}

// FNV-1a over the whole input. Derives the sensor/run-state axes WITHOUT changing the documented
// two-payload framing that seed_gen.cpp mirrors — the fuzzer still steers every bit of it, since
// it controls every byte that feeds the mix.
uint32_t mixOf(const uint8_t *data, size_t size) {
  uint32_t h = 2166136261U;
  for (size_t i = 0; i < size; ++i) {
    h = (h ^ data[i]) * 16777619U;
  }
  return h;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size == 0) {
    return 0;
  }
  const uint32_t mix = mixOf(data, size);

  FakeThermocouples tc;
  tc.workpieceFault = (mix & 1U) != 0U;
  tc.walls = static_cast<int>((mix >> 1) & 3U); // 0..3, so "no wall reference" is reachable
  tc.workpieceC = bitsToFloat(mix * 2654435761U);
  for (int i = 0; i < FakeThermocouples::kWalls; ++i) {
    tc.wallC[i] = bitsToFloat((mix ^ static_cast<uint32_t>(i * 0x9E3779B9U)) * 40503U);
    tc.wallFault[i] = ((mix >> (8 + i)) & 1U) != 0U;
  }

  FakeClock clock;
  ProfileExecutor exec(clock);
  RecipeValidator validator(&tc, &exec);

  // byte 0 = recipe-payload length (clamped to what's available); recipe then Start.
  size_t rlen = data[0];
  if (rlen > size - 1) {
    rlen = size - 1;
  }
  const uint8_t *rbytes = data + 1;
  const uint8_t *sbytes = data + 1 + rlen;
  const size_t slen = size - 1 - rlen;

  bool accepted_reflow = false;
  oven_Recipe recipe = oven_Recipe_init_zero;
  if (protocol::decode(oven_Recipe_fields, &recipe, rbytes, rlen)) {
    oven_NakReason reason = oven_NakReason_NAK_UNSPECIFIED;
    if (validator.validateRecipe(recipe, reason)) {
      FUZZ_ASSERT(recipe.segments_count >= 1);
      bool has_uv = false;
      bool has_motor = false;
      float min_heat = recipe.segments[0].heat_c;
      float max_heat = recipe.segments[0].heat_c;
      for (pb_size_t i = 0; i < recipe.segments_count; ++i) {
        const oven_Segment &s = recipe.segments[i];
        FUZZ_ASSERT(s.dur_ms != 0);
        FUZZ_ASSERT(std::isfinite(s.heat_c));
        has_uv = has_uv || s.uv;
        has_motor = has_motor || s.motor;
        min_heat = s.heat_c < min_heat ? s.heat_c : min_heat;
        max_heat = s.heat_c > max_heat ? s.heat_c : max_heat;
      }
      // A REFLOW-tagged recipe must not assert uv/motor, and every setpoint must sit
      // within [MIN_SEGMENT_C, content-derived hard-max]. Read the untrusted mode tag via
      // wireEnum for the same reason the validator does — an enum-typed load of an out-of-
      // range value is itself UB.
      FUZZ_ASSERT(
          !(protocol::wireEnum(recipe.mode) == oven_Mode_MODE_REFLOW && (has_uv || has_motor)));
      const float cap = oven_safety::hardMaxForMode(oven_safety::deriveMode(recipe));
      FUZZ_ASSERT(max_heat <= cap);
      FUZZ_ASSERT(min_heat >= oven_safety::MIN_SEGMENT_C);
      accepted_reflow = oven_safety::deriveMode(recipe) == oven_Mode_MODE_REFLOW;

      // Sometimes put the executor into a live run before the Start below, so the
      // already-RUNNING guard is exercised rather than merely compiled.
      if ((mix & 0x1000U) != 0U) {
        exec.load(recipe, /*holdEntryGated=*/accepted_reflow);
        exec.start();
      }
    }
  }

  oven_Start start = oven_Start_init_zero;
  if (protocol::decode(oven_Start_fields, &start, sbytes, slen)) {
    oven_NakReason reason = oven_NakReason_NAK_UNSPECIFIED;
    if (validator.validateStart(start, reason)) {
      FUZZ_ASSERT(start.session != 0); // session 0 is the IDLE sentinel, never adoptable
      // Never authorize a second run over a live one, and never start a reflow run blind on its
      // own control sensor. Recomputing the probe verdict here rather than trusting the
      // validator's own call is the point: the two must agree.
      FUZZ_ASSERT(exec.state() != oven_RunState_RUN_STATE_RUNNING);
      if (accepted_reflow) {
        float hottest = oven_domain::kWallRefSeedC;
        for (int i = 0; i < tc.wallCount(); ++i) {
          const TcReading r = tc.wall(i);
          if (!r.fault) {
            hottest = oven_domain::foldWallRef(hottest, r.celsius);
          }
        }
        FUZZ_ASSERT(oven_domain::workpieceTcPlausible(tc.workpieceC, tc.workpieceFault,
                                                      tc.wallCount() > 0, hottest));
      }
    }
  }
  return 0;
}
