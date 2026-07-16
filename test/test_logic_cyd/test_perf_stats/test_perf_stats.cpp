// native_logic_cyd suite — the measurement core the perf harness and the device probe share.
//
// Worth testing precisely because it is a MEASURING instrument: a benchmark whose statistics are
// wrong reports confident nonsense, and nothing downstream would catch it. The percentile rule is
// pinned here so the host's numbers and the board's stay the same numbers.
#include <unity.h>

#include "perf_stats.h"

void setUp(void) {}
void tearDown(void) {}

namespace {
perf::Samples of(const uint32_t *v, size_t n) {
  perf::Samples s;
  for (size_t i = 0; i < n; ++i) {
    s.record(v[i]);
  }
  return s;
}
} // namespace

void test_starts_empty(void) {
  perf::Samples s;
  TEST_ASSERT_TRUE(s.empty());
  TEST_ASSERT_EQUAL_UINT32(0, s.count());
  TEST_ASSERT_EQUAL_UINT32(0, s.dropped());
}

// An empty set has no observations, so every statistic is 0 rather than reading off the end.
void test_empty_percentiles_are_zero(void) {
  perf::Samples s;
  TEST_ASSERT_EQUAL_UINT32(0, s.median());
  TEST_ASSERT_EQUAL_UINT32(0, s.min());
  TEST_ASSERT_EQUAL_UINT32(0, s.max());
  TEST_ASSERT_EQUAL_UINT32(0, s.percentile(95));
}

void test_single_sample_is_every_statistic(void) {
  const uint32_t v[] = {42};
  perf::Samples s = of(v, 1);
  TEST_ASSERT_EQUAL_UINT32(42, s.median());
  TEST_ASSERT_EQUAL_UINT32(42, s.min());
  TEST_ASSERT_EQUAL_UINT32(42, s.max());
}

// Input order must not matter: the harness records in arrival order, not sorted order.
void test_sorts_regardless_of_input_order(void) {
  const uint32_t v[] = {50, 10, 40, 20, 30};
  perf::Samples s = of(v, 5);
  TEST_ASSERT_EQUAL_UINT32(10, s.min());
  TEST_ASSERT_EQUAL_UINT32(50, s.max());
  TEST_ASSERT_EQUAL_UINT32(30, s.median()); // rank ceil(0.5*5)=3 -> idx 2
}

// Nearest-rank on an even count is the LOWER median (no interpolation). Pinned because the
// alternative convention would silently shift every reported number by half a bucket.
void test_median_of_even_count_is_lower_middle(void) {
  const uint32_t v[] = {10, 20, 30, 40};
  perf::Samples s = of(v, 4);
  TEST_ASSERT_EQUAL_UINT32(20, s.median()); // rank ceil(0.5*4)=2 -> idx 1
}

void test_percentiles_are_real_observations(void) {
  uint32_t v[100];
  for (uint32_t i = 0; i < 100; ++i) {
    v[i] = (i + 1) * 10; // 10..1000
  }
  perf::Samples s = of(v, 100);
  TEST_ASSERT_EQUAL_UINT32(10, s.percentile(0));
  TEST_ASSERT_EQUAL_UINT32(50, s.percentile(5));
  TEST_ASSERT_EQUAL_UINT32(500, s.percentile(50));
  TEST_ASSERT_EQUAL_UINT32(950, s.percentile(95));
  TEST_ASSERT_EQUAL_UINT32(1000, s.percentile(100));
}

void test_handles_duplicate_values(void) {
  const uint32_t v[] = {7, 7, 7, 7};
  perf::Samples s = of(v, 4);
  TEST_ASSERT_EQUAL_UINT32(7, s.median());
  TEST_ASSERT_EQUAL_UINT32(7, s.min());
  TEST_ASSERT_EQUAL_UINT32(7, s.max());
}

// Overrun must be visible, not silent. A ring that overwrote would report a run's tail as if it
// were the whole run.
void test_overrun_drops_and_counts(void) {
  perf::Samples s;
  for (uint32_t i = 0; i < perf::Samples::kCapacity + 5; ++i) {
    s.record(i);
  }
  TEST_ASSERT_EQUAL_UINT32(perf::Samples::kCapacity, s.count());
  TEST_ASSERT_EQUAL_UINT32(5, s.dropped());
  TEST_ASSERT_EQUAL_UINT32(0, s.min()); // kept the FIRST kCapacity, not the last
}

void test_reset_clears_counts_and_drops(void) {
  perf::Samples s;
  for (uint32_t i = 0; i < perf::Samples::kCapacity + 3; ++i) {
    s.record(i);
  }
  s.reset();
  TEST_ASSERT_TRUE(s.empty());
  TEST_ASSERT_EQUAL_UINT32(0, s.dropped());
  TEST_ASSERT_EQUAL_UINT32(0, s.median());
}

// Durations are microseconds in a uint32_t; nothing may wrap or sign-flip near the top.
void test_large_values_do_not_overflow(void) {
  const uint32_t big = 4000000000u; // ~4000 s, far past any real sample
  const uint32_t v[] = {1, big, 2};
  perf::Samples s = of(v, 3);
  TEST_ASSERT_EQUAL_UINT32(1, s.min());
  TEST_ASSERT_EQUAL_UINT32(big, s.max());
  TEST_ASSERT_EQUAL_UINT32(2, s.median());
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_starts_empty);
  RUN_TEST(test_empty_percentiles_are_zero);
  RUN_TEST(test_single_sample_is_every_statistic);
  RUN_TEST(test_sorts_regardless_of_input_order);
  RUN_TEST(test_median_of_even_count_is_lower_middle);
  RUN_TEST(test_percentiles_are_real_observations);
  RUN_TEST(test_handles_duplicate_values);
  RUN_TEST(test_overrun_drops_and_counts);
  RUN_TEST(test_reset_clears_counts_and_drops);
  RUN_TEST(test_large_values_do_not_overflow);
  return UNITY_END();
}
