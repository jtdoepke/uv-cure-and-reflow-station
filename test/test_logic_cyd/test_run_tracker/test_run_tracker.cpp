// native_logic_cyd suite — RunTracker (backlog C7/PR2, §15/§16). Drives the live run model with
// synthetic telemetry and pins: the seg_idx -> authored-phase mapping across the compiled recipe,
// the projected-curve interpolation, progress/ETA, the deviation cue sharing the run-fit object,
// and robustness to adversarial telemetry (garbage seg_idx, elapsed rollover, NaN temps). Pure
// logic — no LVGL, no link.
#include <unity.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#include "oven.pb.h"
#include "oven_cal.h"
#include "profile_draft.h"
#include "run_tracker.h"

namespace {

const OvenModel &model() {
  return oven_cal::kDefaultModel;
}
constexpr Caps kReflowCaps{20.0F, 300.0F};
constexpr float kAmbient = 25.0F;

// A 3-phase reflow draft: preheat 150 / soak 180 / reflow 245 (hold 30 s). All ramps ASAP (ramp
// seconds 0), so each phase emits one ramp segment; the last also emits a hold; then the compiler
// appends the implicit cool tail. => segments [p0, p1, p2-ramp, p2-hold, cool].
ProfileDraft reflowDraft() {
  ProfileDraft d{};
  d.mode = RecipeMode::Reflow;
  d.phaseCount = 3;
  std::strncpy(d.phases[0].name, "Preheat", kPhaseNameCap - 1);
  d.phases[0].targetC = 150.0F;
  std::strncpy(d.phases[1].name, "Soak", kPhaseNameCap - 1);
  d.phases[1].targetC = 180.0F;
  std::strncpy(d.phases[2].name, "Reflow", kPhaseNameCap - 1);
  d.phases[2].targetC = 245.0F;
  d.phases[2].holdSeconds = 30.0F;
  return d;
}

oven_Telemetry telem(uint32_t seg, uint32_t elapsedMs, float workC) {
  oven_Telemetry t = oven_Telemetry_init_zero;
  t.seg_idx = seg;
  t.elapsed_ms = elapsedMs;
  t.work_temp = workC;
  t.board_est = workC;
  t.run_state = oven_RunState_RUN_STATE_RUNNING;
  return t;
}

bool finite(float v) {
  return v == v && v < 3.0e38F && v > -3.0e38F;
}

// seg_idx maps back to the authored phase across the compiled recipe (ramp/hold/cool split).
void test_seg_to_phase_mapping() {
  RunTracker rt;
  rt.begin(reflowDraft(), model(), kReflowCaps, kAmbient, 0);
  TEST_ASSERT_EQUAL_UINT32(3, rt.phaseCount());

  rt.update(telem(0, 1000, 60.0F), 1000);
  TEST_ASSERT_EQUAL_UINT32(0, rt.currentPhaseIndex());
  TEST_ASSERT_EQUAL_UINT32(1, rt.phaseOrdinal());
  TEST_ASSERT_EQUAL_STRING("Preheat", rt.phaseName());

  rt.update(telem(1, 2000, 150.0F), 2000);
  TEST_ASSERT_EQUAL_UINT32(1, rt.currentPhaseIndex());
  TEST_ASSERT_EQUAL_STRING("Soak", rt.phaseName());

  // Both segments of phase 2 (ramp then hold) map to phase index 2.
  rt.update(telem(2, 3000, 180.0F), 3000);
  TEST_ASSERT_EQUAL_UINT32(2, rt.currentPhaseIndex());
  rt.update(telem(3, 4000, 245.0F), 4000);
  TEST_ASSERT_EQUAL_UINT32(2, rt.currentPhaseIndex());
  TEST_ASSERT_EQUAL_STRING("Reflow", rt.phaseName());

  // The last segment is the implicit cool tail — no authored phase.
  rt.update(telem(4, 5000, 200.0F), 5000);
  TEST_ASSERT_TRUE(rt.inCooldown());
  TEST_ASSERT_EQUAL_STRING("Cool", rt.phaseName());
}

// projectedAt is finite, bounded, and interpolates: start ~ambient, past-end ~touch-safe cool tail.
void test_projected_curve_interpolation() {
  RunTracker rt;
  rt.begin(reflowDraft(), model(), kReflowCaps, kAmbient, 0);
  TEST_ASSERT_GREATER_THAN_UINT32(0, rt.projectedCount());
  TEST_ASSERT_GREATER_THAN_FLOAT(0.0F, rt.totalSeconds());

  const float atStart = rt.projectedAt(0.0F);
  TEST_ASSERT_FLOAT_WITHIN(1.0F, kAmbient, atStart);

  // Past the end, the projection sits at the cool-tail endpoint (touch-safe, ~43 °C).
  const float atEnd = rt.projectedAt(rt.totalSeconds() + 10000.0F);
  TEST_ASSERT_FLOAT_WITHIN(2.0F, 43.0F, atEnd);

  // A mid-run sample is finite and within the run's temperature span.
  const float mid = rt.projectedAt(rt.totalSeconds() * 0.5F);
  TEST_ASSERT_TRUE(finite(mid));
  TEST_ASSERT_GREATER_OR_EQUAL_FLOAT(kAmbient - 5.0F, mid);
  TEST_ASSERT_LESS_OR_EQUAL_FLOAT(245.0F + 5.0F, mid);
}

// Progress and ETA track elapsed against the projected total, clamped honestly.
void test_progress_and_eta() {
  RunTracker rt;
  rt.begin(reflowDraft(), model(), kReflowCaps, kAmbient, 0);
  const float total = rt.totalSeconds();

  rt.update(telem(0, 0, kAmbient), 0);
  TEST_ASSERT_FLOAT_WITHIN(0.01F, 0.0F, rt.progress01());
  TEST_ASSERT_FLOAT_WITHIN(1.0F, total, rt.etaSeconds());

  const uint32_t halfMs = static_cast<uint32_t>(total * 1000.0F * 0.5F);
  rt.update(telem(2, halfMs, 180.0F), halfMs);
  TEST_ASSERT_FLOAT_WITHIN(0.05F, 0.5F, rt.progress01());

  // Past the projected total: progress pins at 1.0, ETA floors at 0 (behind schedule, honest).
  const uint32_t overMs = static_cast<uint32_t>((total + 300.0F) * 1000.0F);
  rt.update(telem(4, overMs, 60.0F), overMs);
  TEST_ASSERT_EQUAL_FLOAT(1.0F, rt.progress01());
  TEST_ASSERT_EQUAL_FLOAT(0.0F, rt.etaSeconds());
}

// The live deviation cue reads the shared run-fit object: on-track stays quiet, sustained
// off-track trips it, and finish() returns a computed result for a completed run.
void test_deviation_cue_and_finish() {
  RunTracker rt;
  rt.begin(reflowDraft(), model(), kReflowCaps, kAmbient, 0);
  TEST_ASSERT_FALSE(rt.deviating());

  // On-track: feed the projected value as the actual for 20 s of ticks -> never deviates.
  for (uint32_t ms = 0; ms <= 20000; ms += 1000) {
    const float proj = rt.projectedAt(static_cast<float>(ms) / 1000.0F);
    rt.update(telem(0, ms, proj), ms);
  }
  TEST_ASSERT_FALSE(rt.deviating());

  // Off-track: actual far below projected, sustained well past the deviation window -> trips.
  RunTracker rt2;
  rt2.begin(reflowDraft(), model(), kReflowCaps, kAmbient, 0);
  for (uint32_t ms = 0; ms <= 30000; ms += 1000) {
    const float proj = rt2.projectedAt(static_cast<float>(ms) / 1000.0F);
    rt2.update(telem(2, ms, proj - 80.0F), ms);
  }
  TEST_ASSERT_TRUE(rt2.deviating());

  const RunFitResult done = rt2.finish(RunOutcome::Completed, 31000);
  TEST_ASSERT_TRUE(done.computed);
  // A stopped run yields an incomplete (uncomputed) result (§16).
  RunTracker rt3;
  rt3.begin(reflowDraft(), model(), kReflowCaps, kAmbient, 0);
  TEST_ASSERT_FALSE(rt3.finish(RunOutcome::Stopped, 100).computed);
}

// Adversarial telemetry: garbage seg_idx, elapsed rollover, NaN/Inf temps -> every output stays
// finite and in range, no crash, no NaN reaching a would-be lv_line.
void test_adversarial_telemetry() {
  RunTracker rt;
  rt.begin(reflowDraft(), model(), kReflowCaps, kAmbient, 0);

  oven_Telemetry t = oven_Telemetry_init_zero;
  t.seg_idx = 0xFFFFFFFFu;                               // far past the segment count
  t.elapsed_ms = 0xFFFFFFFFu;                            // rollover-scale elapsed
  t.work_temp = std::numeric_limits<float>::quiet_NaN(); // faulted TC
  t.board_est = std::numeric_limits<float>::infinity();
  t.run_state = oven_RunState_RUN_STATE_RUNNING;
  rt.update(t, 1000);

  TEST_ASSERT_TRUE(finite(rt.projectedAt(1.0e9F)));
  TEST_ASSERT_TRUE(finite(rt.progress01()));
  TEST_ASSERT_TRUE(finite(rt.etaSeconds()));
  TEST_ASSERT_TRUE(rt.progress01() >= 0.0F && rt.progress01() <= 1.0F);
  // A garbage seg_idx clamps to the last (cool) segment; the phase model stays valid.
  TEST_ASSERT_TRUE(rt.inCooldown());

  const RunFitResult r = rt.finish(RunOutcome::Completed, 2000);
  TEST_ASSERT_TRUE(finite(r.runQuality.maxAbsC));
  TEST_ASSERT_TRUE(finite(r.runQuality.rmsC));
}

} // namespace

void setUp() {}
void tearDown() {}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_seg_to_phase_mapping);
  RUN_TEST(test_projected_curve_interpolation);
  RUN_TEST(test_progress_and_eta);
  RUN_TEST(test_deviation_cue_and_finish);
  RUN_TEST(test_adversarial_telemetry);
  return UNITY_END();
}
