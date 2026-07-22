// native_control suite — RecipeValidator (A7): the controller's upload-time
// recipe/start checks behind the ISetupValidator seam (design.md §4, §9). Pure
// unit tests; the seq/Ack-Nak wiring around it is covered by test_reliable_setup.
#include <unity.h>

#include <cmath>

#include "helpers/fake_clock.h"
#include "helpers/fake_thermocouples.h"
#include "oven.pb.h"
#include "oven_safety.h"
#include "profile_executor.h"
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

// An out-of-range mode tag off the wire (nanopb stores proto enums raw, so an untrusted
// CYD can send any value) must be read without an enum-typed load — that load is UB, which
// a fuzzer caught on recipe.mode. The bogus tag is simply not REFLOW, so the run is governed
// by content: plain heat is accepted at the reflow cap; uv content still forces the cure cap.
void test_out_of_range_mode_tag_handled(void) {
  RecipeValidator v;
  oven_NakReason reason = oven_NakReason_NAK_UNSPECIFIED;
  const oven_Mode bogus = static_cast<oven_Mode>(120); // a value the enum never defines
  oven_Recipe plain = recipeWith(bogus, 100.0F, /*uv=*/false, false);
  TEST_ASSERT_TRUE(v.validateRecipe(plain, reason));
  oven_Recipe uv_over = recipeWith(bogus, 200.0F, /*uv=*/true, false); // cure cap => rejected
  TEST_ASSERT_FALSE(v.validateRecipe(uv_over, reason));
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

// --- The two Start guards A7 deferred (both blockers landed with A6) ---

// Accept a recipe and return a Start naming it, so the guard cases below reach past the
// known-recipe / session-zero checks that precede them.
static oven_Start acceptAndStart(RecipeValidator &v, const oven_Recipe &rec) {
  oven_NakReason reason = oven_NakReason_NAK_UNSPECIFIED;
  TEST_ASSERT_TRUE(v.validateRecipe(rec, reason));
  oven_Start s = oven_Start_init_default;
  s.session = 7;
  s.recipe_id = rec.id;
  return s;
}

// A Start arriving while the executor is RUNNING is an illegal transition. A genuine retransmit
// never reaches the validator (SetupResponder dedups on seq), so this is a second run request and
// must not silently restart a live run under the operator.
void test_start_while_running_is_illegal_transition(void) {
  FakeClock clk;
  ProfileExecutor exec(clk);
  RecipeValidator v(nullptr, &exec);
  oven_Recipe rec = recipeWith(oven_Mode_MODE_CURE, 80.0F, /*uv=*/true, false);
  rec.id = 5;
  const oven_Start s = acceptAndStart(v, rec);

  // IDLE: the ordinary path, accepted.
  oven_NakReason reason = oven_NakReason_NAK_UNSPECIFIED;
  TEST_ASSERT_TRUE(v.validateStart(s, reason));

  exec.load(rec, /*holdEntryGated=*/false);
  exec.start();
  TEST_ASSERT_EQUAL_INT(oven_RunState_RUN_STATE_RUNNING, exec.state());
  TEST_ASSERT_FALSE(v.validateStart(s, reason));
  TEST_ASSERT_EQUAL_INT(oven_NakReason_NAK_ILLEGAL_TRANSITION, reason);
}

// ...and the guard releases the moment the run ends. This is the case that must not regress: §15's
// cure resume, A11's orphan-end and §16's "Run again" all re-Start after the executor has gone
// IDLE or DONE, and each would break if the guard latched on "a run happened".
void test_start_accepted_again_once_run_ends(void) {
  FakeClock clk;
  ProfileExecutor exec(clk);
  RecipeValidator v(nullptr, &exec);
  oven_Recipe rec = recipeWith(oven_Mode_MODE_CURE, 80.0F, /*uv=*/true, false);
  rec.id = 6;
  const oven_Start s = acceptAndStart(v, rec);

  exec.load(rec, /*holdEntryGated=*/false);
  exec.start();
  oven_NakReason reason = oven_NakReason_NAK_UNSPECIFIED;
  TEST_ASSERT_FALSE(v.validateStart(s, reason));

  exec.abort(); // the door abort / A11 orphan-end / STOP path: back to IDLE
  TEST_ASSERT_EQUAL_INT(oven_RunState_RUN_STATE_IDLE, exec.state());
  TEST_ASSERT_TRUE(v.validateStart(s, reason));
}

// With no executor wired the guard is inert — a build that runs nothing has nothing to collide
// with, and every existing construction site must keep behaving as it did.
void test_no_executor_leaves_transition_guard_inert(void) {
  RecipeValidator v; // neither source
  oven_Recipe rec = recipeWith(oven_Mode_MODE_REFLOW, 200.0F, false, false);
  rec.id = 8;
  const oven_Start s = acceptAndStart(v, rec);
  oven_NakReason reason = oven_NakReason_NAK_UNSPECIFIED;
  TEST_ASSERT_TRUE(v.validateStart(s, reason));
}

// Reflow is CONTROLLED on the workpiece probe (§5), so a Start whose probe reads faulted is
// refused: the run would track nothing. The CYD refuses the same Start in Confirm (§19) off the
// shared predicate; this is the controller holding that line without trusting it.
void test_reflow_start_faulted_probe_invalid(void) {
  FakeThermocouples tc;
  RecipeValidator v(&tc);
  oven_Recipe rec = recipeWith(oven_Mode_MODE_REFLOW, 200.0F, false, false);
  rec.id = 9;
  const oven_Start s = acceptAndStart(v, rec);

  oven_NakReason reason = oven_NakReason_NAK_UNSPECIFIED;
  TEST_ASSERT_TRUE(v.validateStart(s, reason)); // a healthy 25 C probe is fine

  tc.workpieceFault = true;
  TEST_ASSERT_FALSE(v.validateStart(s, reason));
  TEST_ASSERT_EQUAL_INT(oven_NakReason_NAK_WORKPIECE_TC_INVALID, reason);
}

// The same guard over the shapes a broken probe actually presents: NaN, the open-circuit
// out-of-band sentinel, and a reading running implausibly hotter than the chamber (dangling in
// free air rather than clipped to the board — the one the plain band cannot catch).
void test_reflow_start_implausible_probe_invalid(void) {
  FakeThermocouples tc;
  RecipeValidator v(&tc);
  oven_Recipe rec = recipeWith(oven_Mode_MODE_REFLOW, 200.0F, false, false);
  rec.id = 10;
  const oven_Start s = acceptAndStart(v, rec);
  oven_NakReason reason = oven_NakReason_NAK_UNSPECIFIED;

  tc.workpieceC = NAN;
  TEST_ASSERT_FALSE(v.validateStart(s, reason));
  TEST_ASSERT_EQUAL_INT(oven_NakReason_NAK_WORKPIECE_TC_INVALID, reason);

  tc.workpieceC = -300.0F; // open-circuit sentinel, well below the physical floor
  TEST_ASSERT_FALSE(v.validateStart(s, reason));
  TEST_ASSERT_EQUAL_INT(oven_NakReason_NAK_WORKPIECE_TC_INVALID, reason);

  tc.setAll(25.0F);
  tc.workpieceC = 25.0F + oven_domain::kWorkpieceWallSlackC + 5.0F; // way above the walls
  TEST_ASSERT_FALSE(v.validateStart(s, reason));
  TEST_ASSERT_EQUAL_INT(oven_NakReason_NAK_WORKPIECE_TC_INVALID, reason);

  // A COLD workpiece in a warm chamber is the normal case, not a fault — the cross-check is
  // deliberately one-sided.
  tc.setAll(80.0F);
  tc.workpieceC = 25.0F;
  TEST_ASSERT_TRUE(v.validateStart(s, reason));
}

// Cure controls on the chamber wall, not the workpiece (§5), so a dead workpiece probe must not
// block a cure run — the check keys off the recipe's CONTENT-derived mode, not its untrusted tag.
void test_cure_start_unaffected_by_dead_probe(void) {
  FakeThermocouples tc;
  tc.workpieceFault = true;
  RecipeValidator v(&tc);
  oven_Recipe rec = recipeWith(oven_Mode_MODE_CURE, 80.0F, /*uv=*/true, false);
  rec.id = 11;
  const oven_Start s = acceptAndStart(v, rec);
  oven_NakReason reason = oven_NakReason_NAK_UNSPECIFIED;
  TEST_ASSERT_TRUE(v.validateStart(s, reason));

  // And the tag cannot buy a pass: a recipe TAGGED cure but carrying plain heat derives REFLOW,
  // so it is gated on the probe like any other reflow run.
  oven_Recipe mislabelled = recipeWith(oven_Mode_MODE_CURE, 200.0F, false, false);
  mislabelled.id = 12;
  const oven_Start s2 = acceptAndStart(v, mislabelled);
  TEST_ASSERT_FALSE(v.validateStart(s2, reason));
  TEST_ASSERT_EQUAL_INT(oven_NakReason_NAK_WORKPIECE_TC_INVALID, reason);
}

// With no temperature source wired the probe check is skipped entirely — the posture that keeps
// every bare `RecipeValidator v;` (the tests above, fuzz_pipeline.h) behaving as before.
void test_no_tc_source_leaves_probe_guard_inert(void) {
  RecipeValidator v;
  oven_Recipe rec = recipeWith(oven_Mode_MODE_REFLOW, 200.0F, false, false);
  rec.id = 13;
  const oven_Start s = acceptAndStart(v, rec);
  oven_NakReason reason = oven_NakReason_NAK_UNSPECIFIED;
  TEST_ASSERT_TRUE(v.validateStart(s, reason));
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
  RUN_TEST(test_out_of_range_mode_tag_handled);
  RUN_TEST(test_start_requires_known_recipe);
  RUN_TEST(test_start_session_zero_out_of_range);
  RUN_TEST(test_rejected_recipe_not_remembered);
  RUN_TEST(test_start_while_running_is_illegal_transition);
  RUN_TEST(test_start_accepted_again_once_run_ends);
  RUN_TEST(test_no_executor_leaves_transition_guard_inert);
  RUN_TEST(test_reflow_start_faulted_probe_invalid);
  RUN_TEST(test_reflow_start_implausible_probe_invalid);
  RUN_TEST(test_cure_start_unaffected_by_dead_probe);
  RUN_TEST(test_no_tc_source_leaves_probe_guard_inert);
  return UNITY_END();
}
