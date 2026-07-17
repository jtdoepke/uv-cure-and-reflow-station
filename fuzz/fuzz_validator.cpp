// Seam 3: decoded structs → the RecipeValidator backstop, checked directly.
//
// The input is carved into two nanopb payloads by a 1-byte length prefix (the format
// seed_gen.cpp mirrors): byte 0 = L, the next L bytes are decoded as a Recipe, the
// remainder as a Start — both on the same validator, so an accepted Recipe's id can
// satisfy the Start's "known recipe" check. The invariant is accept-implies-
// constraints: when the validator says yes, the message must actually satisfy every
// rule the backstop promises — a validator that accepts what it must reject is the
// bug this hunts.
#include <cmath>

#include "fuzz_util.h"
#include "recipe_validator.h" // pulls codec.h, oven.pb.h, oven_safety.h

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size == 0) {
    return 0;
  }
  RecipeValidator validator;

  // byte 0 = recipe-payload length (clamped to what's available); recipe then Start.
  size_t rlen = data[0];
  if (rlen > size - 1) {
    rlen = size - 1;
  }
  const uint8_t *rbytes = data + 1;
  const uint8_t *sbytes = data + 1 + rlen;
  const size_t slen = size - 1 - rlen;

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
    }
  }

  oven_Start start = oven_Start_init_zero;
  if (protocol::decode(oven_Start_fields, &start, sbytes, slen)) {
    oven_NakReason reason = oven_NakReason_NAK_UNSPECIFIED;
    if (validator.validateStart(start, reason)) {
      FUZZ_ASSERT(start.session != 0); // session 0 is the IDLE sentinel, never adoptable
    }
  }
  return 0;
}
