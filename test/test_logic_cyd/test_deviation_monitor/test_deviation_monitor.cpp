// native_logic_cyd suite — pure host tests of the §15/§16 shared residual detector.
// No LVGL/Arduino: time is passed in, so these tests are also the proof that D6's offline
// replay of a logged run would produce the same numbers as the live 4 Hz feed.
#include <vector>

#include <unity.h>

#include "deviation_monitor.h"

void setUp(void) {}
void tearDown(void) {}

namespace {
constexpr uint32_t kTickMs = 250; // §15's 4 Hz telemetry

// Feed `seconds` worth of samples at 4 Hz holding a constant residual.
void feed(DeviationMonitor &m, uint32_t &t, float residual, uint32_t seconds) {
  for (uint32_t i = 0; i < seconds * 4; ++i) {
    m.addSample(t, 100.0F + residual, 100.0F);
    t += kTickMs;
  }
}
} // namespace

void test_no_samples_is_clean(void) {
  DeviationMonitor m;
  const DeviationMonitor::Stats s = m.stats();
  TEST_ASSERT_EQUAL_UINT32(0, s.sampleCount);
  TEST_ASSERT_EQUAL_FLOAT(0.0F, s.rmsC); // no div-by-zero
  TEST_ASSERT_EQUAL_FLOAT(0.0F, s.meanC);
  TEST_ASSERT_FALSE(s.sustainedExceeded);
  TEST_ASSERT_FALSE(m.deviating());
}

void test_tracks_signed_residual_and_abs_max(void) {
  DeviationMonitor m;
  m.addSample(0, 105.0F, 100.0F);
  TEST_ASSERT_EQUAL_FLOAT(5.0F, m.lastResidualC());
  m.addSample(250, 92.0F, 100.0F); // undershoot
  TEST_ASSERT_EQUAL_FLOAT(-8.0F, m.lastResidualC());
  TEST_ASSERT_EQUAL_FLOAT(8.0F, m.stats().maxAbsC); // max is on |residual|
}

void test_rms_of_a_constant_offset_equals_the_offset(void) {
  DeviationMonitor m;
  uint32_t t = 0;
  feed(m, t, 5.0F, 60);
  const DeviationMonitor::Stats s = m.stats();
  TEST_ASSERT_FLOAT_WITHIN(0.01F, 5.0F, s.rmsC);
  TEST_ASSERT_FLOAT_WITHIN(0.01F, 5.0F, s.meanC);
}

// Guards against summing signed residuals into the RMS accumulator by mistake: a symmetric swing
// has a mean near zero but a real RMS.
void test_rms_of_a_symmetric_swing_beats_the_mean(void) {
  DeviationMonitor m;
  uint32_t t = 0;
  for (uint32_t i = 0; i < 240; ++i) {
    const float r = (i % 2 == 0) ? 6.0F : -6.0F;
    m.addSample(t, 100.0F + r, 100.0F);
    t += kTickMs;
  }
  const DeviationMonitor::Stats s = m.stats();
  TEST_ASSERT_FLOAT_WITHIN(0.2F, 0.0F, s.meanC);
  TEST_ASSERT_FLOAT_WITHIN(0.2F, 6.0F, s.rmsC);
}

void test_within_band_never_deviates(void) {
  DeviationMonitor m; // band 10 °C
  uint32_t t = 0;
  feed(m, t, 9.5F, 120); // two minutes just inside the band
  TEST_ASSERT_FALSE(m.outOfBand());
  TEST_ASSERT_FALSE(m.deviating());
  TEST_ASSERT_FALSE(m.stats().sustainedExceeded);
  TEST_ASSERT_EQUAL_UINT32(0, m.stats().totalOutOfBandMs);
}

// §16's headline qualifier: "RMSE / deviation beyond a band **sustained > N s** (sustained, so a
// transient door-open spike doesn't trip it)". This is the door-open case.
void test_transient_spike_does_not_trip_the_sustained_flag(void) {
  DeviationMonitor m; // band 10 °C, sustained 15 s
  uint32_t t = 0;
  feed(m, t, 0.0F, 60); // a minute tracking perfectly
  feed(m, t, 30.0F, 2); // a 2-second +30 °C spike — the door cracks
  feed(m, t, 0.0F, 60); // recovers
  const DeviationMonitor::Stats s = m.stats();
  TEST_ASSERT_FALSE(m.deviating());
  TEST_ASSERT_FALSE(s.sustainedExceeded);    // 2 s << 15 s
  TEST_ASSERT_EQUAL_FLOAT(30.0F, s.maxAbsC); // but the spike is still reported
  TEST_ASSERT_TRUE(s.totalOutOfBandMs > 0);
}

void test_sustained_excursion_trips_at_exactly_n_seconds(void) {
  DeviationMonitor::Config cfg;
  cfg.sustainedMs = 15000;
  DeviationMonitor m(cfg);
  uint32_t t = 0;
  m.addSample(t, 130.0F, 100.0F); // seed the excursion at t=0
  t += kTickMs;
  // Accumulate to 14750 ms of excursion — one tick short.
  for (; t < 15000; t += kTickMs) {
    m.addSample(t, 130.0F, 100.0F);
  }
  TEST_ASSERT_FALSE(m.deviating());
  m.addSample(t, 130.0F, 100.0F); // t == 15000 → exactly N
  TEST_ASSERT_TRUE(m.deviating());
  TEST_ASSERT_TRUE(m.stats().sustainedExceeded);
}

// longestExcursionMs is the longest CONTINUOUS stretch, not the total — otherwise two brief
// door-opens would masquerade as one sustained miss.
void test_excursion_resets_when_it_returns_to_band(void) {
  DeviationMonitor m;
  uint32_t t = 0;
  feed(m, t, 30.0F, 10); // 10 s out
  feed(m, t, 0.0F, 10);  // back in band
  feed(m, t, 30.0F, 10); // 10 s out again
  const DeviationMonitor::Stats s = m.stats();
  TEST_ASSERT_UINT32_WITHIN(500, 10000, s.longestExcursionMs); // not 20000
  TEST_ASSERT_UINT32_WITHIN(500, 20000, s.totalOutOfBandMs);
  TEST_ASSERT_FALSE(s.sustainedExceeded); // neither stretch reached 15 s
}

// §15: the cue fires for a run "falling behind / overshooting" — the band is symmetric.
void test_symmetric_band_catches_overshoot_and_lag(void) {
  DeviationMonitor over;
  uint32_t t1 = 0;
  feed(over, t1, 30.0F, 30);
  TEST_ASSERT_TRUE(over.deviating());

  DeviationMonitor under;
  uint32_t t2 = 0;
  feed(under, t2, -30.0F, 30);
  TEST_ASSERT_TRUE(under.deviating());
}

// What dt-weighting actually buys, and the reason it isn't optional: a run whose sample RATE
// changes partway. Here the first half is dense (4 Hz, tracking perfectly) and the second half
// sparse (1 Hz, 10 °C off) — equal *durations*, unequal sample counts.
//
// Weighted by time (correct): half the run at 0, half at 10 → rms = sqrt(50) ≈ 7.07.
// Counting samples (wrong): 120 zeros vs 30 tens → rms = sqrt(100*30/150) ≈ 4.47, which would
// quietly under-report the miss in proportion to how densely we happened to log it.
//
// Rate changes are real: §7's SD ring buffer drains opportunistically and D6 may decimate a log.
void test_dt_weighting_survives_a_rate_change_mid_run(void) {
  DeviationMonitor m;
  uint32_t t = 0;
  for (; t < 30000; t += 250) { // 4 Hz, on target
    m.addSample(t, 100.0F, 100.0F);
  }
  for (; t <= 60000; t += 1000) { // 1 Hz, 10 °C high
    m.addSample(t, 110.0F, 100.0F);
  }
  TEST_ASSERT_FLOAT_WITHIN(0.25F, 7.07F, m.stats().rmsC); // time-weighted, not sample-counted
  TEST_ASSERT_FLOAT_WITHIN(0.25F, 5.0F, m.stats().meanC);
}

// The same physical run logged at two rates yields the same statistics — the property D6's
// offline replay depends on.
//
// Uses a smooth ramp deliberately. A step discontinuity CANNOT be rate-independent under any
// causal rule: a 1 Hz log genuinely doesn't know where inside its 1 s interval the step fell, so
// it misattributes up to that interval. That's sampling, not a defect — but it means a step
// would be testing the sample grid rather than the weighting.
void test_replay_is_rate_independent_on_a_smooth_signal(void) {
  DeviationMonitor fast;
  for (uint32_t t = 0; t <= 60000; t += 250) { // 4 Hz: residual ramps 0 → 10 °C
    fast.addSample(t, 100.0F + static_cast<float>(t) / 6000.0F, 100.0F);
  }
  DeviationMonitor slow;
  for (uint32_t t = 0; t <= 60000; t += 1000) { // 1 Hz: same run, coarser log
    slow.addSample(t, 100.0F + static_cast<float>(t) / 6000.0F, 100.0F);
  }
  TEST_ASSERT_FLOAT_WITHIN(0.1F, fast.stats().rmsC, slow.stats().rmsC);
  TEST_ASSERT_FLOAT_WITHIN(0.1F, fast.stats().meanC, slow.stats().meanC);
  TEST_ASSERT_FLOAT_WITHIN(0.05F, 5.77F, fast.stats().rmsC); // ≈ 10/√3, the exact ramp RMS
}

// An SD stall (§7 notes 100–500 ms, occasionally >1 s) or a paused run must not let one sample
// carry a minute of weight.
void test_sample_gap_is_clamped(void) {
  DeviationMonitor::Config cfg;
  cfg.maxSampleGapMs = 2000;
  DeviationMonitor m(cfg);
  m.addSample(0, 100.0F, 100.0F);
  m.addSample(60000, 130.0F, 100.0F); // a 60 s gap, clamped to 2 s
  const DeviationMonitor::Stats s = m.stats();
  TEST_ASSERT_EQUAL_UINT32(2000, s.totalOutOfBandMs);
  TEST_ASSERT_FALSE(s.sustainedExceeded); // 2 s, not 60 s → no false sustained trip
}

// THE test for the §15/§16 sharing claim: there is one implementation, so a live feed and an
// offline replay of the recorded samples are bit-for-bit identical — not merely close.
void test_batch_replay_matches_streaming(void) {
  struct Sample {
    uint32_t t;
    float actual;
    float projected;
  };
  std::vector<Sample> log;
  DeviationMonitor live;
  uint32_t t = 0;
  for (uint32_t i = 0; i < 400; ++i) {
    const float actual = 100.0F + static_cast<float>((i * 7) % 41) - 20.0F; // deterministic churn
    live.addSample(t, actual, 100.0F);  // §15: fed live, one tick at a time
    log.push_back({t, actual, 100.0F}); // B8·1 writes the same samples to SD
    t += kTickMs;
  }
  DeviationMonitor replay; // D6's offline decoder, or a re-read of the log
  for (const Sample &s : log) {
    replay.addSample(s.t, s.actual, s.projected);
  }
  const DeviationMonitor::Stats a = live.stats();
  const DeviationMonitor::Stats b = replay.stats();
  TEST_ASSERT_EQUAL_UINT32(a.sampleCount, b.sampleCount);
  TEST_ASSERT_EQUAL_UINT32(a.spanMs, b.spanMs);
  TEST_ASSERT_EQUAL_FLOAT(a.maxAbsC, b.maxAbsC);
  TEST_ASSERT_EQUAL_FLOAT(a.rmsC, b.rmsC);
  TEST_ASSERT_EQUAL_FLOAT(a.meanC, b.meanC);
  TEST_ASSERT_EQUAL_UINT32(a.longestExcursionMs, b.longestExcursionMs);
  TEST_ASSERT_EQUAL_UINT32(a.totalOutOfBandMs, b.totalOutOfBandMs);
  TEST_ASSERT_EQUAL(a.sustainedExceeded, b.sustainedExceeded);
}

void test_reset_clears_everything(void) {
  DeviationMonitor m;
  uint32_t t = 0;
  feed(m, t, 30.0F, 30);
  TEST_ASSERT_TRUE(m.deviating());
  m.reset();
  TEST_ASSERT_FALSE(m.deviating());
  TEST_ASSERT_FALSE(m.outOfBand());
  const DeviationMonitor::Stats s = m.stats();
  TEST_ASSERT_EQUAL_UINT32(0, s.sampleCount);
  TEST_ASSERT_EQUAL_FLOAT(0.0F, s.maxAbsC);
  TEST_ASSERT_EQUAL_UINT32(0, s.longestExcursionMs);
}

// The config must survive reset() — it is policy, not state.
void test_reset_preserves_config(void) {
  DeviationMonitor::Config cfg;
  cfg.bandC = 2.0F;
  DeviationMonitor m(cfg);
  m.reset();
  m.addSample(0, 105.0F, 100.0F); // 5 °C — outside the tightened 2 °C band
  TEST_ASSERT_TRUE(m.outOfBand());
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_no_samples_is_clean);
  RUN_TEST(test_tracks_signed_residual_and_abs_max);
  RUN_TEST(test_rms_of_a_constant_offset_equals_the_offset);
  RUN_TEST(test_rms_of_a_symmetric_swing_beats_the_mean);
  RUN_TEST(test_within_band_never_deviates);
  RUN_TEST(test_transient_spike_does_not_trip_the_sustained_flag);
  RUN_TEST(test_sustained_excursion_trips_at_exactly_n_seconds);
  RUN_TEST(test_excursion_resets_when_it_returns_to_band);
  RUN_TEST(test_symmetric_band_catches_overshoot_and_lag);
  RUN_TEST(test_dt_weighting_survives_a_rate_change_mid_run);
  RUN_TEST(test_replay_is_rate_independent_on_a_smooth_signal);
  RUN_TEST(test_sample_gap_is_clamped);
  RUN_TEST(test_batch_replay_matches_streaming);
  RUN_TEST(test_reset_clears_everything);
  RUN_TEST(test_reset_preserves_config);
  return UNITY_END();
}
