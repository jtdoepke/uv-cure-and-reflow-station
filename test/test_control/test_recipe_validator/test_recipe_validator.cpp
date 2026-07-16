// native_control suite — RecipeValidator (A7): the controller's upload-time
// recipe/start checks behind the ISetupValidator seam (design.md §4, §9). Pure
// unit tests; the seq/Ack-Nak wiring around it is covered by test_reliable_setup.
#include <unity.h>

#include <cmath>

#include "oven.pb.h"
#include "oven_safety.h"
#include "recipe_validator.h"

// Build a one-segment recipe; callers tweak fields for the case under test.
static oven_Recipe recipeWith(oven_Mode mode, float heat_c, bool uv, bool motor) {
  oven_Recipe rec = oven_Recipe_init_default;
  rec.id = 1;
  rec.mode = mode;
  rec.segments_count = 1;
  rec.segments[0].dur_ms = 1000;
  rec.segments[0].heat_c = heat_c;
  rec.segments[0].uv = uv;
  rec.segments[0].motor = motor;
  rec.segments[0].interp = oven_Interp_INTERP_HOLD;
  return rec;
}

static bool accepts(RecipeValidator &v, const oven_Recipe &rec, oven_NakReason &reason) {
  reason = oven_NakReason_NAK_UNSPECIFIED;
  return v.validateRecipe(rec, reason);
}

void setUp(void) {}
void tearDown(void) {}

// A recipe with no segments is rejected out-of-range.
void test_empty_recipe_out_of_range(void) {
  RecipeValidator v;
  oven_Recipe rec = oven_Recipe_init_default;
  rec.id = 1;
  rec.mode = oven_Mode_MODE_REFLOW;
  rec.segments_count = 0;
  oven_NakReason reason = oven_NakReason_NAK_UNSPECIFIED;
  TEST_ASSERT_FALSE(v.validateRecipe(rec, reason));
  TEST_ASSERT_EQUAL_INT(oven_NakReason_NAK_OUT_OF_RANGE, reason);
}

// A zero-duration segment is rejected out-of-range.
void test_zero_duration_out_of_range(void) {
  RecipeValidator v;
  oven_Recipe rec = recipeWith(oven_Mode_MODE_REFLOW, 150.0F, false, false);
  rec.segments[0].dur_ms = 0;
  oven_NakReason reason = oven_NakReason_NAK_UNSPECIFIED;
  TEST_ASSERT_FALSE(v.validateRecipe(rec, reason));
  TEST_ASSERT_EQUAL_INT(oven_NakReason_NAK_OUT_OF_RANGE, reason);
}

// A reflow-tagged recipe asserting UV is a mode/content mismatch.
void test_reflow_tag_with_uv_is_mismatch(void) {
  RecipeValidator v;
  oven_Recipe rec = recipeWith(oven_Mode_MODE_REFLOW, 80.0F, /*uv=*/true, false);
  oven_NakReason reason = oven_NakReason_NAK_UNSPECIFIED;
  TEST_ASSERT_FALSE(v.validateRecipe(rec, reason));
  TEST_ASSERT_EQUAL_INT(oven_NakReason_NAK_MODE_CONTENT_MISMATCH, reason);
}

// A reflow-tagged recipe asserting the motor is a mode/content mismatch.
void test_reflow_tag_with_motor_is_mismatch(void) {
  RecipeValidator v;
  oven_Recipe rec = recipeWith(oven_Mode_MODE_REFLOW, 80.0F, false, /*motor=*/true);
  oven_NakReason reason = oven_NakReason_NAK_UNSPECIFIED;
  TEST_ASSERT_FALSE(v.validateRecipe(rec, reason));
  TEST_ASSERT_EQUAL_INT(oven_NakReason_NAK_MODE_CONTENT_MISMATCH, reason);
}

// UV mixed with a setpoint above the cure ceiling is a mode/content mismatch
// (§9), distinct from a plain out-of-range setpoint.
void test_uv_above_cure_ceiling_is_mismatch(void) {
  RecipeValidator v;
  const float too_hot = oven_safety::CURE_HARD_MAX_C + 10.0F;
  oven_Recipe rec = recipeWith(oven_Mode_MODE_CURE, too_hot, /*uv=*/true, false);
  oven_NakReason reason = oven_NakReason_NAK_UNSPECIFIED;
  TEST_ASSERT_FALSE(v.validateRecipe(rec, reason));
  TEST_ASSERT_EQUAL_INT(oven_NakReason_NAK_MODE_CONTENT_MISMATCH, reason);
}

// A plain-heat reflow segment above the reflow hard-max is out-of-range.
void test_reflow_above_hard_max_out_of_range(void) {
  RecipeValidator v;
  const float too_hot = oven_safety::REFLOW_HARD_MAX_C + 10.0F;
  oven_Recipe rec = recipeWith(oven_Mode_MODE_REFLOW, too_hot, false, false);
  oven_NakReason reason = oven_NakReason_NAK_UNSPECIFIED;
  TEST_ASSERT_FALSE(v.validateRecipe(rec, reason));
  TEST_ASSERT_EQUAL_INT(oven_NakReason_NAK_OUT_OF_RANGE, reason);
}

// A sub-floor (negative) setpoint is out-of-range.
void test_below_floor_out_of_range(void) {
  RecipeValidator v;
  oven_Recipe rec = recipeWith(oven_Mode_MODE_REFLOW, -5.0F, false, false);
  oven_NakReason reason = oven_NakReason_NAK_UNSPECIFIED;
  TEST_ASSERT_FALSE(v.validateRecipe(rec, reason));
  TEST_ASSERT_EQUAL_INT(oven_NakReason_NAK_OUT_OF_RANGE, reason);
}

// A non-finite (NaN) setpoint is rejected out-of-range: every magnitude guard
// compares false against NaN, so the finiteness check is what stops it. The
// validator is the untrusted-CYD backstop; heat_c arrives as a raw wire float.
void test_nan_setpoint_out_of_range(void) {
  RecipeValidator v;
  oven_Recipe rec = recipeWith(oven_Mode_MODE_REFLOW, NAN, false, false);
  oven_NakReason reason = oven_NakReason_NAK_UNSPECIFIED;
  TEST_ASSERT_FALSE(v.validateRecipe(rec, reason));
  TEST_ASSERT_EQUAL_INT(oven_NakReason_NAK_OUT_OF_RANGE, reason);
}

// A valid reflow recipe (plain heat within the reflow ceiling) is accepted.
void test_valid_reflow_accepted(void) {
  RecipeValidator v;
  oven_Recipe rec = recipeWith(oven_Mode_MODE_REFLOW, 250.0F, false, false);
  oven_NakReason reason = oven_NakReason_NAK_UNSPECIFIED;
  TEST_ASSERT_TRUE(v.validateRecipe(rec, reason));
}

// A valid cure recipe (UV on, within the cure ceiling) is accepted.
void test_valid_cure_accepted(void) {
  RecipeValidator v;
  oven_Recipe rec = recipeWith(oven_Mode_MODE_CURE, 90.0F, /*uv=*/true, false);
  oven_NakReason reason = oven_NakReason_NAK_UNSPECIFIED;
  TEST_ASSERT_TRUE(v.validateRecipe(rec, reason));
}

// A cure recipe above the reflow ceiling is rejected: uv content selects the
// tighter cure cap regardless of setpoint magnitude.
void test_cure_content_selects_tight_cap(void) {
  RecipeValidator v;
  // Between the cure and reflow ceilings: fine as reflow, over-range as cure.
  const float between = (oven_safety::CURE_HARD_MAX_C + oven_safety::REFLOW_HARD_MAX_C) / 2.0F;
  oven_NakReason reason = oven_NakReason_NAK_UNSPECIFIED;
  // Same setpoint, motor content (no uv) => cure cap => rejected out-of-range.
  oven_Recipe cure = recipeWith(oven_Mode_MODE_CURE, between, false, /*motor=*/true);
  TEST_ASSERT_FALSE(v.validateRecipe(cure, reason));
  TEST_ASSERT_EQUAL_INT(oven_NakReason_NAK_OUT_OF_RANGE, reason);
  // Same setpoint, plain heat => reflow cap => accepted.
  oven_Recipe reflow = recipeWith(oven_Mode_MODE_REFLOW, between, false, false);
  TEST_ASSERT_TRUE(v.validateRecipe(reflow, reason));
}

// validateStart accepts only the most recently accepted recipe id.
void test_start_requires_known_recipe(void) {
  RecipeValidator v;
  oven_NakReason reason = oven_NakReason_NAK_UNSPECIFIED;

  // No recipe accepted yet: any Start is unknown.
  oven_Start s = oven_Start_init_default;
  s.session = 7;
  s.recipe_id = 1;
  TEST_ASSERT_FALSE(v.validateStart(s, reason));
  TEST_ASSERT_EQUAL_INT(oven_NakReason_NAK_UNKNOWN_RECIPE, reason);

  // Accept recipe id 1, then Start of id 1 is authorized.
  oven_Recipe rec = recipeWith(oven_Mode_MODE_REFLOW, 200.0F, false, false);
  rec.id = 1;
  TEST_ASSERT_TRUE(accepts(v, rec, reason));
  TEST_ASSERT_TRUE(v.validateStart(s, reason));

  // A Start for a different id is rejected as unknown.
  oven_Start other = oven_Start_init_default;
  other.session = 7;
  other.recipe_id = 99;
  TEST_ASSERT_FALSE(v.validateStart(other, reason));
  TEST_ASSERT_EQUAL_INT(oven_NakReason_NAK_UNKNOWN_RECIPE, reason);
}

// A rejected recipe does not become the known recipe (no side effect on Nak).
void test_rejected_recipe_not_remembered(void) {
  RecipeValidator v;
  oven_NakReason reason = oven_NakReason_NAK_UNSPECIFIED;

  oven_Recipe bad = recipeWith(oven_Mode_MODE_REFLOW, 999.0F, false, false);
  bad.id = 42;
  TEST_ASSERT_FALSE(accepts(v, bad, reason));

  oven_Start s = oven_Start_init_default;
  s.session = 7; // non-zero: session 0 is rejected before the recipe check
  s.recipe_id = 42;
  TEST_ASSERT_FALSE(v.validateStart(s, reason));
  TEST_ASSERT_EQUAL_INT(oven_NakReason_NAK_UNKNOWN_RECIPE, reason);
}

// A Start naming session 0 is rejected out-of-range: 0 is the IDLE telemetry
// sentinel and must never be adopted as a live run, even if the recipe is known.
void test_start_session_zero_out_of_range(void) {
  RecipeValidator v;
  oven_NakReason reason = oven_NakReason_NAK_UNSPECIFIED;

  oven_Recipe rec = recipeWith(oven_Mode_MODE_REFLOW, 200.0F, false, false);
  rec.id = 3;
  TEST_ASSERT_TRUE(accepts(v, rec, reason));

  oven_Start s = oven_Start_init_default;
  s.session = 0;
  s.recipe_id = 3; // known recipe, but session 0 is still rejected
  TEST_ASSERT_FALSE(v.validateStart(s, reason));
  TEST_ASSERT_EQUAL_INT(oven_NakReason_NAK_OUT_OF_RANGE, reason);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_empty_recipe_out_of_range);
  RUN_TEST(test_zero_duration_out_of_range);
  RUN_TEST(test_reflow_tag_with_uv_is_mismatch);
  RUN_TEST(test_reflow_tag_with_motor_is_mismatch);
  RUN_TEST(test_uv_above_cure_ceiling_is_mismatch);
  RUN_TEST(test_reflow_above_hard_max_out_of_range);
  RUN_TEST(test_below_floor_out_of_range);
  RUN_TEST(test_nan_setpoint_out_of_range);
  RUN_TEST(test_valid_reflow_accepted);
  RUN_TEST(test_valid_cure_accepted);
  RUN_TEST(test_cure_content_selects_tight_cap);
  RUN_TEST(test_start_requires_known_recipe);
  RUN_TEST(test_start_session_zero_out_of_range);
  RUN_TEST(test_rejected_recipe_not_remembered);
  return UNITY_END();
}
