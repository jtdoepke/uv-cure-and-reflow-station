// native_logic_cyd suite — the planned calibration-sweep generator (calibration_sweep.h; backlog
// B9, design.md §5/§7/§20). The load-bearing property, shared with test_recipe_compiler and
// test_profile_templates: every run the generator emits compiles hardValid and satisfies the
// invariants the controller's RecipeValidator (A7) enforces, so a characterization run never NAKs
// on upload. Plus the sweep's own structure (staircase shape, both fan states, cool-only runs) and
// determinism.
#include <cmath>
#include <cstring>

#include <unity.h>

#include "calibration_sweep.h"
#include "oven_cal.h"
#include "recipe_compiler.h"

void setUp(void) {}
void tearDown(void) {}

namespace {

const Caps kReflowCaps{0.0f, 300.0f};

// Reproduce (without linking) the controller-side RecipeValidator (A7) checks — identical to
// test_recipe_compiler's helper: ≥1 segment, every dur_ms > 0 and heat_c finite, within
// [0, content-derived hard-max], and a reflow tag asserts no uv/motor.
void assertUploadable(const oven_Recipe &rec) {
  TEST_ASSERT_TRUE(rec.segments_count >= 1);
  bool hasUv = false, hasMotor = false;
  float maxHeat = rec.segments[0].heat_c, minHeat = rec.segments[0].heat_c;
  for (pb_size_t i = 0; i < rec.segments_count; ++i) {
    const oven_Segment &s = rec.segments[i];
    TEST_ASSERT_TRUE(s.dur_ms > 0);
    TEST_ASSERT_TRUE(std::isfinite(s.heat_c));
    hasUv = hasUv || s.uv;
    hasMotor = hasMotor || s.motor;
    maxHeat = s.heat_c > maxHeat ? s.heat_c : maxHeat;
    minHeat = s.heat_c < minHeat ? s.heat_c : minHeat;
  }
  if (rec.mode == oven_Mode_MODE_REFLOW) {
    TEST_ASSERT_FALSE(hasUv);
    TEST_ASSERT_FALSE(hasMotor);
  }
  const float cap = (hasUv || hasMotor) ? 120.0f : 300.0f;
  TEST_ASSERT_TRUE(maxHeat <= cap);
  TEST_ASSERT_TRUE(minHeat >= 0.0f);
}

// Compile a generated run exactly as the run path would (mode Reflow, uncalibrated default model),
// against the SAME caps the generator clamped to — the airtight side of the differential.
CompileResult compileRun(const ProfileDraft &d, Caps caps) {
  return compileRecipe(d.phases, d.phaseCount, RecipeMode::Reflow, oven_cal::kDefaultModel, caps,
                       cal_sweep::kNominalAmbientC, /*id=*/1, /*seq=*/1);
}

// Every run of a scope must generate, carry no uv/motor, stay within caps, and compile uploadable.
void assertScopeUploadable(cal_sweep::Scope scope, Caps caps) {
  const cal_sweep::Grid g = cal_sweep::gridFor(scope);
  const size_t rc = cal_sweep::runCount(g);
  TEST_ASSERT_TRUE(rc >= 1);
  for (size_t i = 0; i < rc; ++i) {
    ProfileDraft d{};
    TEST_ASSERT_TRUE(cal_sweep::generateRun(g, i, caps, d));
    TEST_ASSERT_TRUE(d.mode == RecipeMode::Reflow);
    TEST_ASSERT_TRUE(d.phaseCount >= 1 && d.phaseCount <= kMaxPhases);
    for (size_t k = 0; k < d.phaseCount; ++k) {
      TEST_ASSERT_FALSE(d.phases[k].uv);
      TEST_ASSERT_FALSE(d.phases[k].motor);
      TEST_ASSERT_TRUE(std::isfinite(d.phases[k].targetC));
      TEST_ASSERT_TRUE(d.phases[k].targetC >= caps.minC && d.phases[k].targetC <= caps.capC);
    }
    const CompileResult r = compileRun(d, caps);
    TEST_ASSERT_TRUE(r.hardValid);
    assertUploadable(r.recipe);
  }
}

bool sameDraft(const ProfileDraft &a, const ProfileDraft &b) {
  if (std::strncmp(a.name, b.name, kProfileNameCap) != 0)
    return false;
  if (a.mode != b.mode || a.stock != b.stock || a.phaseCount != b.phaseCount)
    return false;
  for (size_t k = 0; k < a.phaseCount; ++k) {
    const Phase &x = a.phases[k];
    const Phase &y = b.phases[k];
    if (std::strncmp(x.name, y.name, kPhaseNameCap) != 0)
      return false;
    if (x.targetC != y.targetC || x.rampSeconds != y.rampSeconds || x.holdSeconds != y.holdSeconds)
      return false;
    if (x.exposurePerSurface != y.exposurePerSurface)
      return false;
    if (x.uv != y.uv || x.motor != y.motor || x.convFan != y.convFan)
      return false;
  }
  return true;
}

} // namespace

void test_every_run_uploadable_all_scopes(void) {
  assertScopeUploadable(cal_sweep::Scope::Quick, kReflowCaps);
  assertScopeUploadable(cal_sweep::Scope::Standard, kReflowCaps);
  assertScopeUploadable(cal_sweep::Scope::Thorough, kReflowCaps);
}

// A user cap tighter than the top bands must still yield uploadable runs — the high bands are
// dropped, not clamped-and-collapsed into an invalid recipe.
void test_tight_cap_drops_high_bands(void) {
  const Caps tight{0.0f, 100.0f}; // Standard bands 80/150/220 → only 80 survives
  assertScopeUploadable(cal_sweep::Scope::Standard, tight);
  ProfileDraft d{};
  const cal_sweep::Grid g = cal_sweep::gridFor(cal_sweep::Scope::Standard);
  TEST_ASSERT_TRUE(cal_sweep::generateRun(g, 0, tight, d)); // first staircase run
  TEST_ASSERT_EQUAL_UINT32(1, d.phaseCount);
  TEST_ASSERT_EQUAL_FLOAT(80.0f, d.phases[0].targetC);
}

// Degenerate caps (inverted range / no band fits) → every run is refused, never a bad recipe.
void test_degenerate_caps_refused(void) {
  const cal_sweep::Grid g = cal_sweep::gridFor(cal_sweep::Scope::Standard);
  const Caps inverted{200.0f, 50.0f};
  const Caps belowAll{0.0f, 40.0f}; // every band > 40
  for (size_t i = 0; i < cal_sweep::runCount(g); ++i) {
    ProfileDraft d{};
    TEST_ASSERT_FALSE(cal_sweep::generateRun(g, i, inverted, d));
    TEST_ASSERT_FALSE(cal_sweep::generateRun(g, i, belowAll, d));
  }
}

// runCount is the exact number of buildable runs; one past the end refuses.
void test_run_count_boundary(void) {
  const cal_sweep::Grid g = cal_sweep::gridFor(cal_sweep::Scope::Thorough);
  // Thorough: fan-off + fan-on staircases + 3 cool-only = 5.
  TEST_ASSERT_EQUAL_UINT32(5, cal_sweep::runCount(g));
  ProfileDraft d{};
  TEST_ASSERT_TRUE(cal_sweep::generateRun(g, cal_sweep::runCount(g) - 1, kReflowCaps, d));
  TEST_ASSERT_FALSE(cal_sweep::generateRun(g, cal_sweep::runCount(g), kReflowCaps, d));
}

// Both fan states appear across the staircase runs, and the staircase climbs (ascending bands).
void test_both_fan_states_and_ascending(void) {
  const cal_sweep::Grid g = cal_sweep::gridFor(cal_sweep::Scope::Standard);
  ProfileDraft off{}, on{};
  TEST_ASSERT_TRUE(cal_sweep::generateRun(g, 0, kReflowCaps, off)); // fan-off staircase
  TEST_ASSERT_TRUE(cal_sweep::generateRun(g, 1, kReflowCaps, on));  // fan-on staircase

  TEST_ASSERT_TRUE(off.phases[0].convFan == FanMode::Off);
  TEST_ASSERT_TRUE(on.phases[0].convFan == FanMode::On);

  // Ascending, ASAP ramps, positive holds.
  TEST_ASSERT_TRUE(off.phaseCount >= 2);
  for (size_t k = 0; k < off.phaseCount; ++k) {
    TEST_ASSERT_EQUAL_FLOAT(0.0f, off.phases[k].rampSeconds);
    TEST_ASSERT_TRUE(off.phases[k].holdSeconds > 0.0f);
    if (k > 0)
      TEST_ASSERT_TRUE(off.phases[k].targetC > off.phases[k - 1].targetC);
  }
}

// The cool-only runs come after the staircases, are single-phase, and run the fan off (passive).
void test_cool_only_runs(void) {
  const cal_sweep::Grid g = cal_sweep::gridFor(cal_sweep::Scope::Thorough);
  const size_t fanRuns = cal_sweep::fanRunCount(g);
  for (size_t i = fanRuns; i < cal_sweep::runCount(g); ++i) {
    ProfileDraft d{};
    TEST_ASSERT_TRUE(cal_sweep::generateRun(g, i, kReflowCaps, d));
    TEST_ASSERT_EQUAL_UINT32(1, d.phaseCount);
    TEST_ASSERT_TRUE(d.phases[0].convFan == FanMode::Off);
    TEST_ASSERT_EQUAL_FLOAT(220.0f, d.phases[0].targetC); // top usable band
  }
}

// Determinism: the same (grid, i, caps) always yields byte-equal drafts — required so a logged
// run-id reproduces the exact profile offline.
void test_determinism(void) {
  const cal_sweep::Grid g = cal_sweep::gridFor(cal_sweep::Scope::Thorough);
  for (size_t i = 0; i < cal_sweep::runCount(g); ++i) {
    ProfileDraft a{}, b{};
    TEST_ASSERT_TRUE(cal_sweep::generateRun(g, i, kReflowCaps, a));
    TEST_ASSERT_TRUE(cal_sweep::generateRun(g, i, kReflowCaps, b));
    TEST_ASSERT_TRUE(sameDraft(a, b));
  }
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_every_run_uploadable_all_scopes);
  RUN_TEST(test_tight_cap_drops_high_bands);
  RUN_TEST(test_degenerate_caps_refused);
  RUN_TEST(test_run_count_boundary);
  RUN_TEST(test_both_fan_states_and_ascending);
  RUN_TEST(test_cool_only_runs);
  RUN_TEST(test_determinism);
  return UNITY_END();
}
