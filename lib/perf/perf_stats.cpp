#include "perf_stats.h"

namespace perf {

void Samples::reset() {
  n_ = 0;
  dropped_ = 0;
}

void Samples::record(uint32_t us) {
  if (n_ >= kCapacity) {
    ++dropped_;
    return;
  }
  v_[n_++] = us;
}

uint32_t Samples::percentile(uint8_t pct) const {
  if (n_ == 0) {
    return 0;
  }

  // Insertion sort on a stack copy. n_ <= 128 and this runs once per report, never per sample,
  // so the O(n^2) is thousands of comparisons on a 240 MHz core — far below the noise floor of
  // what we are measuring. Hand-rolled rather than <algorithm> to keep the header's
  // dependency-free promise literal.
  uint32_t s[kCapacity];
  for (size_t i = 0; i < n_; ++i) {
    uint32_t x = v_[i];
    size_t j = i;
    while (j > 0 && s[j - 1] > x) {
      s[j] = s[j - 1];
      --j;
    }
    s[j] = x;
  }

  if (pct >= 100) {
    return s[n_ - 1];
  }
  // ceil(pct * n / 100) in integer arithmetic, then to a 0-based index. pct==0 yields rank 0,
  // which clamps to the minimum.
  size_t rank = (static_cast<size_t>(pct) * n_ + 99) / 100;
  size_t idx = rank == 0 ? 0 : rank - 1;
  return s[idx];
}

} // namespace perf
