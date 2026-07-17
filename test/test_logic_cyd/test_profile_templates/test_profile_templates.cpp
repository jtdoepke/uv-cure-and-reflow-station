// native_logic_cyd suite — the fixed per-mode phase templates (C5,
// lib/app_logic/profile_templates.h). A seeded template must be a hard-valid compileRecipe() input
// against the factory caps, so create-from-scratch lands the operator on a profile that already
// uploads cleanly; and the role labels must track the canonical/edited structure (§12).
#include <cstring>

#include <unity.h>

#include "oven_cal.h"
#include "profile_templates.h"
#include "recipe_compiler.h"
#include "settings_defaults.h"

void setUp(void) {}
void tearDown(void) {}

// The caps a fresh oven enforces at the editor: factory user caps, floor at MIN_SEGMENT_C (0).
static Caps reflowCaps() {
  return Caps{0.0f, static_cast<float>(settings_defaults::REFLOW_CAP_DEFAULT)};
}
static Caps cureCaps() {
  return Caps{0.0f, static_cast<float>(settings_defaults::UV_CAP_DEFAULT)};
}

void test_reflow_template_is_hard_valid(void) {
  const ProfileStore::StoredProfile t = profile_templates::defaultTemplate(RecipeMode::Reflow);
  TEST_ASSERT_EQUAL_UINT(profile_templates::kReflowPhases, t.phaseCount);
  TEST_ASSERT_EQUAL_INT((int)RecipeMode::Reflow, (int)t.mode);
  TEST_ASSERT_FALSE(t.stock);
  const CompileResult r = compileRecipe(t.phases, t.phaseCount, RecipeMode::Reflow,
                                        oven_cal::kDefaultModel, reflowCaps(), 25.0f, 1, 1);
  TEST_ASSERT_TRUE(r.hardValid);
  TEST_ASSERT_EQUAL_INT((int)CompileReject::None, (int)r.reject);
}

void test_cure_template_is_hard_valid(void) {
  const ProfileStore::StoredProfile t = profile_templates::defaultTemplate(RecipeMode::Cure);
  TEST_ASSERT_EQUAL_UINT(profile_templates::kCurePhases, t.phaseCount);
  TEST_ASSERT_EQUAL_INT((int)RecipeMode::Cure, (int)t.mode);
  const CompileResult r = compileRecipe(t.phases, t.phaseCount, RecipeMode::Cure,
                                        oven_cal::kDefaultModel, cureCaps(), 25.0f, 1, 1);
  TEST_ASSERT_TRUE(r.hardValid);
}

// The cure template's UV/motor phase must stay cure-only content — a reflow compile of it must be
// rejected (content-derived cap, §4), proving the template is genuinely cure-shaped.
void test_cure_template_has_uv_content(void) {
  const ProfileStore::StoredProfile t = profile_templates::defaultTemplate(RecipeMode::Cure);
  bool anyUv = false;
  for (size_t i = 0; i < t.phaseCount; ++i) {
    anyUv = anyUv || t.phases[i].uv;
  }
  TEST_ASSERT_TRUE(anyUv);
}

void test_role_labels_track_structure(void) {
  char buf[24];
  // Canonical reflow → fixed roles (Preheat/Soak/Reflow; the cool-down is implicit, no "Cool"
  // role).
  TEST_ASSERT_EQUAL_STRING(
      "Preheat", profile_templates::phaseLabel(RecipeMode::Reflow, 0,
                                               profile_templates::kReflowPhases, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING("Reflow", profile_templates::phaseLabel(
                                         RecipeMode::Reflow, profile_templates::kReflowPhases - 1,
                                         profile_templates::kReflowPhases, buf, sizeof(buf)));
  // Canonical cure → fixed roles (Warm/Cure).
  TEST_ASSERT_EQUAL_STRING("Cure", profile_templates::phaseLabel(RecipeMode::Cure, 1,
                                                                 profile_templates::kCurePhases,
                                                                 buf, sizeof(buf)));
  // Edited (non-canonical count) → generic "Phase N".
  TEST_ASSERT_EQUAL_STRING(
      "Phase 2", profile_templates::phaseLabel(RecipeMode::Reflow, 1, 5, buf, sizeof(buf)));
}

void test_blank_phase_is_valid_input(void) {
  Phase p = profile_templates::blankPhase();
  const CompileResult r =
      compileRecipe(&p, 1, RecipeMode::Reflow, oven_cal::kDefaultModel, reflowCaps(), 25.0f, 1, 1);
  TEST_ASSERT_TRUE(r.hardValid);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_reflow_template_is_hard_valid);
  RUN_TEST(test_cure_template_is_hard_valid);
  RUN_TEST(test_cure_template_has_uv_content);
  RUN_TEST(test_role_labels_track_structure);
  RUN_TEST(test_blank_phase_is_valid_input);
  return UNITY_END();
}
