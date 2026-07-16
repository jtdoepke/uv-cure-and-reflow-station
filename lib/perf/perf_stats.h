// perf_stats — the measurement core shared by the host harness (perf/) and the on-device probe
// (src_cyd/perf_probe.cpp), so a host number and a device number are computed by the same code
// and can honestly be compared.
//
// Deliberately dependency-free (<cstddef>/<cstdint> only, its own lib like lib/panel): no LVGL,
// no Arduino, no LovyanGFX, no board identity. That is what lets it compile for the firmware and
// for the native targets alike, and what makes it unit-testable on the host.
//
// Allocation-free by construction: a fixed ring plus a stack copy to sort. A measurement tool
// that mallocs is measuring its own allocator, and on a board with a 64 kB LVGL pool and no
// PSRAM it would also perturb the thing under test.
#pragma once

#include <cstddef>
#include <cstdint>

namespace perf {

// A fixed-capacity sample set of microsecond durations.
//
// Reports ORDER STATISTICS (median/percentiles), never a mean: one scheduler hiccup or one cold
// flash-cache miss drags a mean somewhere no sample ever was, which is exactly the failure mode
// a benchmark on a shared laptop and a WiFi-capable MCU is prone to.
class Samples {
public:
  // 128 covers the harness's N=50 with headroom. Sized so the sort's stack copy (512 B) is
  // comfortable inside the ESP32 loop task's 8 kB.
  static constexpr size_t kCapacity = 128;

  void reset();
  void record(uint32_t us);

  size_t count() const { return n_; }
  bool empty() const { return n_ == 0; }
  // Samples past kCapacity are DROPPED, not overwritten, and counted here. A ring that silently
  // overwrote would report the tail of a run as if it were the whole run; a caller that outruns
  // the buffer should find out.
  size_t dropped() const { return dropped_; }

  // Nearest-rank (ceil) percentile: index = ceil(pct/100 * n) - 1 over the sorted samples. So
  // every value returned is a real observation, and p50 on an even count is the lower median.
  // Integer-only and exact — no interpolation to argue about across two architectures.
  uint32_t percentile(uint8_t pct) const;
  uint32_t median() const { return percentile(50); }
  uint32_t min() const { return percentile(0); }
  uint32_t max() const { return percentile(100); }

private:
  uint32_t v_[kCapacity] = {};
  size_t n_ = 0;
  size_t dropped_ = 0;
};

} // namespace perf
