// native_logic_cyd suite — the §15 cure resume generator (B6). The profile that finishes an
// interrupted cure: an ASAP re-heat to the interrupted phase's target, its REMAINING hold/dose,
// then the untouched phases after it. Pure domain logic, no LVGL and no clock.
#include <cstring>

#include <unity.h>

#include "profile_templates.h"
#include "remainder.h"

void setUp(void) {}
void tearDown(void) {}

// A three-phase cure: warm → cure (the long UV soak) → cool.
static ProfileDraft cureDraft() {
  ProfileDraft d{};
  std::strncpy(d.name, "Resin-A", kProfileNameCap - 1);
  d.mode = RecipeMode::Cure;
  d.phaseCount = 3;

  std::strncpy(d.phases[0].name, "Warm", kPhaseNameCap - 1);
  d.phases[0].targetC = 45.0f;
  d.phases[0].rampSeconds = 300.0f;
  d.phases[0].holdSeconds = 60.0f;
  d.phases[0].exposurePerSurface = 0.0f;

  std::strncpy(d.phases[1].name, "Cure", kPhaseNameCap - 1);
  d.phases[1].targetC = 60.0f;
  d.phases[1].rampSeconds = 240.0f;
  d.phases[1].holdSeconds = 600.0f;
  d.phases[1].exposurePerSurface = 400.0f;
  d.phases[1].uv = true;
  d.phases[1].motor = true;

  std::strncpy(d.phases[2].name, "Cool", kPhaseNameCap - 1);
  d.phases[2].targetC = 30.0f;
  d.phases[2].rampSeconds = 600.0f;
  return d;
}

// Interrupted 25% into the cure soak: the remainder re-heats ASAP to that phase's target and
// carries 75% of both the dose and the fallback seconds, then the phases after it.
void test_midphase_scales_remaining_hold_and_dose(void) {
  const ProfileDraft src = cureDraft();
  ProfileDraft out{};
  TEST_ASSERT_TRUE(cure_resume::build(src, /*phaseIndex=*/1, /*holdDelivered01=*/0.25f, out));

  TEST_ASSERT_EQUAL_UINT32(2, out.phaseCount); // the rest of Cure, then Cool
  TEST_ASSERT_EQUAL_STRING("Cure", out.phases[0].name);
  TEST_ASSERT_EQUAL_FLOAT(60.0f, out.phases[0].targetC);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, out.phases[0].rampSeconds);          // §15: RAMP_ASAP re-heat
  TEST_ASSERT_EQUAL_FLOAT(300.0f, out.phases[0].exposurePerSurface); // 75% of 400
  TEST_ASSERT_EQUAL_FLOAT(450.0f, out.phases[0].holdSeconds);        // 75% of 600
  TEST_ASSERT_TRUE(out.phases[0].uv);
  TEST_ASSERT_TRUE(out.phases[0].motor);

  // The phases after it are carried through untouched — including their authored ramp times.
  TEST_ASSERT_EQUAL_STRING("Cool", out.phases[1].name);
  TEST_ASSERT_EQUAL_FLOAT(30.0f, out.phases[1].targetC);
  TEST_ASSERT_EQUAL_FLOAT(600.0f, out.phases[1].rampSeconds);

  // Same job, so the same name and mode; never a library entry.
  TEST_ASSERT_EQUAL_STRING("Resin-A", out.name);
  TEST_ASSERT_EQUAL_INT((int)RecipeMode::Cure, (int)out.mode);
  TEST_ASSERT_FALSE(out.stock);
}

// Interrupted before the soak began (still ramping): the whole phase is redone.
void test_nothing_delivered_redoes_whole_phase(void) {
  const ProfileDraft src = cureDraft();
  ProfileDraft out{};
  TEST_ASSERT_TRUE(cure_resume::build(src, 1, 0.0f, out));
  TEST_ASSERT_EQUAL_FLOAT(400.0f, out.phases[0].exposurePerSurface);
  TEST_ASSERT_EQUAL_FLOAT(600.0f, out.phases[0].holdSeconds);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, out.phases[0].rampSeconds); // still an ASAP re-heat
}

// A phase all-but-finished advances to the NEXT one rather than re-heating for a sliver of soak —
// all the thermal cost of a phase for none of its effect.
void test_almost_complete_phase_starts_at_next(void) {
  const ProfileDraft src = cureDraft();
  ProfileDraft out{};
  TEST_ASSERT_TRUE(cure_resume::build(src, 1, 0.995f, out));

  TEST_ASSERT_EQUAL_UINT32(1, out.phaseCount);
  TEST_ASSERT_EQUAL_STRING("Cool", out.phases[0].name);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, out.phases[0].rampSeconds); // the new first phase is ASAP too
}

// The last phase, finished → nothing to resume.
void test_last_phase_complete_has_no_remainder(void) {
  const ProfileDraft src = cureDraft();
  ProfileDraft out{};
  TEST_ASSERT_FALSE(cure_resume::build(src, 2, 1.0f, out));
}

// §15: "reflow → aborted; no resume (reflow can't survive the thermal excursion — decided)".
// Refuse rather than quietly produce a profile the design says must not exist.
void test_reflow_never_resumes(void) {
  ProfileDraft src = profile_templates::defaultTemplate(RecipeMode::Reflow);
  ProfileDraft out{};
  TEST_ASSERT_FALSE(cure_resume::build(src, 1, 0.5f, out));
}

// Out-of-range phase (the run was in its cool tail, or never entered a phase) → nothing to resume.
void test_out_of_range_phase_rejected(void) {
  const ProfileDraft src = cureDraft();
  ProfileDraft out{};
  TEST_ASSERT_FALSE(cure_resume::build(src, 99, 0.5f, out));
  ProfileDraft empty{};
  empty.mode = RecipeMode::Cure;
  TEST_ASSERT_FALSE(cure_resume::build(empty, 0, 0.0f, out));
}

// A non-finite or out-of-band fraction clamps toward REDOING the phase: an under-exposed part is
// scrapped, an over-exposed one usually is not, so that is the safe direction to round.
void test_bad_fraction_clamps_conservatively(void) {
  const ProfileDraft src = cureDraft();
  ProfileDraft out{};
  const float nan = 0.0f / 0.0f;
  TEST_ASSERT_TRUE(cure_resume::build(src, 1, nan, out));
  TEST_ASSERT_EQUAL_FLOAT(400.0f, out.phases[0].exposurePerSurface); // treated as 0 delivered

  TEST_ASSERT_TRUE(cure_resume::build(src, 1, -5.0f, out));
  TEST_ASSERT_EQUAL_FLOAT(400.0f, out.phases[0].exposurePerSurface);

  // >1 delivered is "finished", which advances to the next phase.
  TEST_ASSERT_TRUE(cure_resume::build(src, 1, 7.0f, out));
  TEST_ASSERT_EQUAL_STRING("Cool", out.phases[0].name);
}

// Resuming the FIRST phase keeps every later phase — the common "opened the door early" case.
void test_first_phase_keeps_all_later_phases(void) {
  const ProfileDraft src = cureDraft();
  ProfileDraft out{};
  TEST_ASSERT_TRUE(cure_resume::build(src, 0, 0.5f, out));
  TEST_ASSERT_EQUAL_UINT32(3, out.phaseCount);
  TEST_ASSERT_EQUAL_STRING("Warm", out.phases[0].name);
  TEST_ASSERT_EQUAL_FLOAT(30.0f, out.phases[0].holdSeconds); // half of 60
  TEST_ASSERT_EQUAL_STRING("Cure", out.phases[1].name);
  TEST_ASSERT_EQUAL_FLOAT(600.0f, out.phases[1].holdSeconds); // untouched
  TEST_ASSERT_EQUAL_FLOAT(240.0f, out.phases[1].rampSeconds); // untouched
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_midphase_scales_remaining_hold_and_dose);
  RUN_TEST(test_nothing_delivered_redoes_whole_phase);
  RUN_TEST(test_almost_complete_phase_starts_at_next);
  RUN_TEST(test_last_phase_complete_has_no_remainder);
  RUN_TEST(test_reflow_never_resumes);
  RUN_TEST(test_out_of_range_phase_rejected);
  RUN_TEST(test_bad_fraction_clamps_conservatively);
  RUN_TEST(test_first_phase_keeps_all_later_phases);
  return UNITY_END();
}
