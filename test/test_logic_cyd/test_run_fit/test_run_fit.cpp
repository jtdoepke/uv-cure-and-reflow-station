// native_logic_cyd suite — pure host tests of §16's fit verdict + calibration-drift advisory.
// The headline cases are the two discrimination branches: §16's whole claim is that the two
// residuals separate "the oven drifted" from "the model was wrong about this board".
#include <cstring>
#include <type_traits>

#include <unity.h>

#include "run_fit.h"

void setUp(void) {}
void tearDown(void) {}

namespace {
constexpr uint32_t kTickMs = 250; // §15's 4 Hz telemetry

// Feed `seconds` of 4 Hz telemetry with fixed offsets from a 100 °C projection.
// workOffset drives the run-quality channel; estOffset drives the estimator channel.
void feed(RunFitAccumulator &rf, uint32_t &t, float workOffset, float estOffset, uint32_t seconds) {
  for (uint32_t i = 0; i < seconds * 4; ++i) {
    const float work = 100.0F + workOffset;
    rf.addSample(t, work, 100.0F, work + estOffset);
    t += kTickMs;
  }
}
} // namespace

// §16: "On a **completed** run only (abort/fault skip it — data incomplete)".
void test_stopped_run_computes_nothing(void) {
  RunFitAccumulator rf;
  uint32_t t = 0;
  rf.beginRun(t);
  feed(rf, t, 30.0F, 30.0F, 60); // wildly off — but the data is incomplete, so it doesn't count
  const RunFitResult r = rf.finish(t, RunOutcome::Stopped);
  TEST_ASSERT_FALSE(r.computed);
  TEST_ASSERT_FALSE(r.advisory);
  TEST_ASSERT_EQUAL(DriftCause::None, r.cause);
  TEST_ASSERT_EQUAL_UINT32(0, r.runQuality.sampleCount);
}

void test_fault_run_computes_nothing(void) {
  RunFitAccumulator rf;
  uint32_t t = 0;
  rf.beginRun(t);
  feed(rf, t, 30.0F, 0.0F, 60);
  TEST_ASSERT_FALSE(rf.finish(t, RunOutcome::Fault).computed);
}

void test_clean_completed_run_is_good_none_no_advisory(void) {
  RunFitAccumulator rf;
  uint32_t t = 0;
  rf.beginRun(t);
  rf.beginPhase(t, /*targetC=*/100.0F, /*requiredHoldMs=*/10000);
  feed(rf, t, 0.0F, 0.0F, 60);
  const RunFitResult r = rf.finish(t, RunOutcome::Completed);
  TEST_ASSERT_TRUE(r.computed);
  TEST_ASSERT_EQUAL(FitVerdict::Good, r.verdict);
  TEST_ASSERT_EQUAL(DriftCause::None, r.cause);
  TEST_ASSERT_FALSE(r.advisory);
  TEST_ASSERT_EQUAL_UINT8(0, r.phasesMissedTarget);
  TEST_ASSERT_EQUAL_UINT8(0, r.phasesShortHold);
}

// §16: "A large `boardEst`-vs-`workTemp` residual points at the projection model (board-mass
// mismatch or estimator drift)". The run tracked the plan exactly; only the estimate is wrong.
void test_perfect_tracking_with_drifting_estimator_flags_projection_model(void) {
  RunFitAccumulator rf;
  uint32_t t = 0;
  rf.beginRun(t);
  rf.beginPhase(t, 100.0F, 10000);
  feed(rf, t, 0.0F, /*estOffset=*/15.0F, 60); // actual == projected; boardEst 15 °C off truth
  const RunFitResult r = rf.finish(t, RunOutcome::Completed);
  TEST_ASSERT_TRUE(r.computed);
  TEST_ASSERT_EQUAL(DriftCause::ProjectionModel, r.cause);
  TEST_ASSERT_TRUE(r.advisory);
  TEST_ASSERT_TRUE(r.estimatorQuality.sustainedExceeded);
  TEST_ASSERT_FALSE(r.runQuality.sustainedExceeded);
  TEST_ASSERT_EQUAL(FitVerdict::Good, r.verdict); // the RUN was fine; the model wasn't
}

// §16: "a measured-vs-projected miss with a *clean* estimator residual points at the oven itself
// (element aging, door seal)". The discrimination headline.
void test_projection_miss_with_clean_estimator_flags_the_oven(void) {
  RunFitAccumulator rf;
  uint32_t t = 0;
  rf.beginRun(t);
  rf.beginPhase(t, 100.0F, 10000);
  feed(rf, t, /*workOffset=*/-15.0F, /*estOffset=*/0.0F, 60); // lagging; boardEst == workTemp
  const RunFitResult r = rf.finish(t, RunOutcome::Completed);
  TEST_ASSERT_EQUAL(DriftCause::Oven, r.cause);
  TEST_ASSERT_TRUE(r.advisory);
  TEST_ASSERT_TRUE(r.runQuality.sustainedExceeded);
  TEST_ASSERT_FALSE(r.estimatorQuality.sustainedExceeded);
  TEST_ASSERT_EQUAL(FitVerdict::Poor, r.verdict);
}

// When both are dirty the estimator wins the attribution: a drifting estimator explains a
// projection miss, so blaming the oven would send the operator after the wrong cause.
void test_both_dirty_attributes_to_the_projection_model(void) {
  RunFitAccumulator rf;
  uint32_t t = 0;
  rf.beginRun(t);
  rf.beginPhase(t, 100.0F, 10000);
  feed(rf, t, -15.0F, 15.0F, 60);
  const RunFitResult r = rf.finish(t, RunOutcome::Completed);
  TEST_ASSERT_EQUAL(DriftCause::ProjectionModel, r.cause);
  TEST_ASSERT_TRUE(r.advisory);
}

// The §16 sustained qualifier at the run_fit level: a door-open transient must not summon the
// recalibration advisory on an otherwise good run.
void test_door_open_spike_alone_does_not_raise_the_advisory(void) {
  RunFitAccumulator rf;
  uint32_t t = 0;
  rf.beginRun(t);
  rf.beginPhase(t, 100.0F, 10000);
  feed(rf, t, 0.0F, 0.0F, 60);
  feed(rf, t, -30.0F, 0.0F, 2); // 2 s door crack
  feed(rf, t, 0.0F, 0.0F, 60);
  const RunFitResult r = rf.finish(t, RunOutcome::Completed);
  TEST_ASSERT_FALSE(r.advisory);
  TEST_ASSERT_EQUAL(DriftCause::None, r.cause);
  TEST_ASSERT_EQUAL(FitVerdict::Poor, r.verdict); // maxAbs 30 > poorMaxC 20 — reported, not blamed
}

// §16's "per-phase target checks — did soak/peak actually reach target".
void test_missed_peak_target_forces_poor(void) {
  RunFitAccumulator rf;
  uint32_t t = 0;
  rf.beginRun(t);
  rf.beginPhase(t, /*targetC=*/245.0F, /*requiredHoldMs=*/10000);
  for (uint32_t i = 0; i < 240; ++i) {       // 60 s at 238 °C — 7 °C short, tolerance is 3
    rf.addSample(t, 238.0F, 238.0F, 238.0F); // projection tracks, so only the target check fails
    t += kTickMs;
  }
  rf.endPhase(t);
  const RunFitResult r = rf.finish(t, RunOutcome::Completed);
  TEST_ASSERT_EQUAL_UINT8(1, r.phasesMissedTarget);
  TEST_ASSERT_FALSE(rf.phase(0).reachedTarget);
  TEST_ASSERT_EQUAL(FitVerdict::Poor, r.verdict);
  TEST_ASSERT_EQUAL(DriftCause::Oven, r.cause); // reached nothing, estimator clean → the oven
}

void test_short_hold_is_fair_not_poor(void) {
  RunFitAccumulator rf;
  uint32_t t = 0;
  rf.beginRun(t);
  rf.beginPhase(t, /*targetC=*/100.0F, /*requiredHoldMs=*/30000); // wants 30 s above target
  feed(rf, t, 0.0F, 0.0F, 10);                                    // only 10 s
  rf.endPhase(t);
  const RunFitResult r = rf.finish(t, RunOutcome::Completed);
  TEST_ASSERT_TRUE(rf.phase(0).reachedTarget);
  TEST_ASSERT_FALSE(rf.phase(0).heldLongEnough);
  TEST_ASSERT_EQUAL_UINT8(0, r.phasesMissedTarget);
  TEST_ASSERT_EQUAL_UINT8(1, r.phasesShortHold);
  TEST_ASSERT_EQUAL(FitVerdict::Fair, r.verdict);
}

// Time-above-liquidus is the SUM of the above-target spans: a momentary dip doesn't reset the
// hold, it just doesn't count toward it.
void test_time_above_liquidus_accumulates_across_a_dip(void) {
  RunFitAccumulator rf;
  uint32_t t = 0;
  rf.beginRun(t);
  rf.beginPhase(t, /*targetC=*/100.0F, /*requiredHoldMs=*/20000);
  feed(rf, t, 0.0F, 0.0F, 12);  // 12 s above
  feed(rf, t, -20.0F, 0.0F, 5); // dips below target
  feed(rf, t, 0.0F, 0.0F, 12);  // 12 s above again → 24 s total > 20 s required
  rf.endPhase(t);
  TEST_ASSERT_TRUE(rf.phase(0).heldLongEnough);
  TEST_ASSERT_UINT32_WITHIN(1000, 24000, rf.phase(0).timeAtOrAboveMs); // not 29000
}

void test_target_tolerance_boundary(void) {
  RunFitAccumulator rf;
  uint32_t t = 0;
  rf.beginRun(t);
  rf.beginPhase(t, /*targetC=*/100.0F, /*requiredHoldMs=*/0);
  rf.addSample(t, 97.0F, 97.0F, 97.0F); // exactly targetC - targetToleranceC
  t += kTickMs;
  rf.endPhase(t);
  TEST_ASSERT_TRUE(rf.phase(0).reachedTarget); // the boundary counts as reached
}

void test_phase_count_saturates_at_max_phases(void) {
  RunFitAccumulator rf;
  uint32_t t = 0;
  rf.beginRun(t);
  for (uint32_t i = 0; i < 40; ++i) { // more than MAX_PHASES (32) — must not overrun
    rf.beginPhase(t, 100.0F, 0);
    rf.addSample(t, 100.0F, 100.0F, 100.0F);
    t += kTickMs;
  }
  const RunFitResult r = rf.finish(t, RunOutcome::Completed);
  TEST_ASSERT_EQUAL_UINT8(RunFitAccumulator::MAX_PHASES, r.phaseCount);
}

// The §15/§16 consistency property: the live cue and the summary flag are the same statement,
// because they are the same object. If the readout went amber during the run, the summary says so.
void test_live_cue_matches_the_summary_flag(void) {
  RunFitAccumulator rf;
  uint32_t t = 0;
  rf.beginRun(t);
  rf.beginPhase(t, 100.0F, 0);
  bool cueFiredDuringRun = false;
  for (uint32_t i = 0; i < 400; ++i) {
    rf.addSample(t, 70.0F, 100.0F, 70.0F); // sustained 30 °C lag
    cueFiredDuringRun = cueFiredDuringRun || rf.liveDeviationCue();
    t += kTickMs;
  }
  const RunFitResult r = rf.finish(t, RunOutcome::Completed);
  TEST_ASSERT_TRUE(cueFiredDuringRun);
  TEST_ASSERT_EQUAL(cueFiredDuringRun, r.runQuality.sustainedExceeded);
}

void test_live_cue_stays_dark_on_a_clean_run(void) {
  RunFitAccumulator rf;
  uint32_t t = 0;
  rf.beginRun(t);
  rf.beginPhase(t, 100.0F, 0);
  bool cueFired = false;
  for (uint32_t i = 0; i < 400; ++i) {
    rf.addSample(t, 100.0F, 100.0F, 100.0F);
    cueFired = cueFired || rf.liveDeviationCue();
    t += kTickMs;
  }
  const RunFitResult r = rf.finish(t, RunOutcome::Completed);
  TEST_ASSERT_FALSE(cueFired);
  TEST_ASSERT_FALSE(r.runQuality.sustainedExceeded);
}

void test_begin_run_clears_a_previous_run(void) {
  RunFitAccumulator rf;
  uint32_t t = 0;
  rf.beginRun(t);
  rf.beginPhase(t, 100.0F, 0);
  feed(rf, t, -30.0F, 0.0F, 60);
  rf.beginRun(t); // a second run on the same object
  rf.beginPhase(t, 100.0F, 0);
  feed(rf, t, 0.0F, 0.0F, 60);
  const RunFitResult r = rf.finish(t, RunOutcome::Completed);
  TEST_ASSERT_EQUAL(FitVerdict::Good, r.verdict);
  TEST_ASSERT_FALSE(r.advisory);
  TEST_ASSERT_EQUAL_UINT8(1, r.phaseCount);
}

void test_advisory_text_is_nonempty_for_every_drift_cause(void) {
  TEST_ASSERT_TRUE(strlen(advisoryText(DriftCause::Oven)) > 0);
  TEST_ASSERT_TRUE(strlen(advisoryText(DriftCause::ProjectionModel)) > 0);
  TEST_ASSERT_EQUAL_STRING("", advisoryText(DriftCause::None)); // nothing to say
}

// §16: "the advisory picks its wording accordingly instead of guessing" — the two causes must
// not share one string, or the discrimination is decorative.
void test_advisory_text_differs_per_cause(void) {
  TEST_ASSERT_NOT_EQUAL(
      0, strcmp(advisoryText(DriftCause::Oven), advisoryText(DriftCause::ProjectionModel)));
}

// B8·1 memcpy's this into the SD log header (§7).
void test_result_is_trivially_copyable(void) {
  static_assert(std::is_trivially_copyable<RunFitResult>::value,
                "RunFitResult goes into the SD log header (§7) — keep it POD");
  TEST_PASS();
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_stopped_run_computes_nothing);
  RUN_TEST(test_fault_run_computes_nothing);
  RUN_TEST(test_clean_completed_run_is_good_none_no_advisory);
  RUN_TEST(test_perfect_tracking_with_drifting_estimator_flags_projection_model);
  RUN_TEST(test_projection_miss_with_clean_estimator_flags_the_oven);
  RUN_TEST(test_both_dirty_attributes_to_the_projection_model);
  RUN_TEST(test_door_open_spike_alone_does_not_raise_the_advisory);
  RUN_TEST(test_missed_peak_target_forces_poor);
  RUN_TEST(test_short_hold_is_fair_not_poor);
  RUN_TEST(test_time_above_liquidus_accumulates_across_a_dip);
  RUN_TEST(test_target_tolerance_boundary);
  RUN_TEST(test_phase_count_saturates_at_max_phases);
  RUN_TEST(test_live_cue_matches_the_summary_flag);
  RUN_TEST(test_live_cue_stays_dark_on_a_clean_run);
  RUN_TEST(test_begin_run_clears_a_previous_run);
  RUN_TEST(test_advisory_text_is_nonempty_for_every_drift_cause);
  RUN_TEST(test_advisory_text_differs_per_cause);
  RUN_TEST(test_result_is_trivially_copyable);
  return UNITY_END();
}
