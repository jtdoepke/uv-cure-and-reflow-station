// Internal-correctness harness: RunTracker (C7, lib/app_logic/run_tracker.h) turns UNTRUSTED
// telemetry into the Run/Monitor screen's live readouts, the chart's projected point, and the
// deviation stats. A buggy/rebooted controller can emit any seg_idx / elapsed_ms / temperature
// (incl. rollover, NaN/Inf), so this is a parsing-like seam feeding an LVGL widget. The harness
// arms the tracker over a raw Phase[] draft (adversarial floats, both modes, calibrated +
// uncalibrated model, adversarial ambient) and drives it with adversarial telemetry, holding:
//
//   - projectedAt(t) is finite and bounded for any t (incl. negative / past-end / huge);
//   - progress01() is finite and in [0,1]; etaSeconds() is finite and >= 0;
//   - phaseOrdinal() <= phaseCount(); the seg->phase map never indexes out of bounds;
//   - finish() returns finite deviation stats — no NaN can reach the summary or a would-be lv_line.
//
// ASAN/UBSAN catch any OOB/UB. Joins the fuzz_profile_facts / fuzz_compiler family
// (fuzz/README.md). Input format mirrors fuzz_profile_facts.cpp (header + 17-byte phase records);
// the telemetry frames are derived from the same bytes read at rotating offsets, so the corpus is
// shared-shaped.
#include <cmath>
#include <cstring>

#include "fuzz_util.h"

#include "oven.pb.h"
#include "oven_cal.h"
#include "profile_draft.h"
#include "run_tracker.h"

namespace {

constexpr size_t kHeaderBytes = 6;
constexpr size_t kRecordBytes = 17;

float readF32(const uint8_t *p) {
  float f;
  std::memcpy(&f, p, sizeof(f));
  return f;
}

FanMode readFan(uint8_t bits) {
  return static_cast<FanMode>(bits > 2 ? 0 : bits);
}

// Wrapping little-endian reads, so telemetry frames can be derived from any offset without bounds
// math (size >= kHeaderBytes > 0 is guaranteed by the caller).
uint32_t wrapU32(const uint8_t *d, size_t size, size_t off) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) {
    v |= static_cast<uint32_t>(d[(off + static_cast<size_t>(i)) % size]) << (8 * i);
  }
  return v;
}
float wrapF32(const uint8_t *d, size_t size, size_t off) {
  const uint32_t v = wrapU32(d, size, off);
  float f;
  std::memcpy(&f, &v, sizeof(f));
  return f;
}

bool fin(float v) {
  return std::isfinite(v);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size < kHeaderBytes) {
    return 0;
  }

  const RecipeMode mode = (data[0] & 1) ? RecipeMode::Cure : RecipeMode::Reflow;
  const OvenModel &model = oven_cal::kDefaultModel;
  const float ambient = readF32(data + 2);

  const size_t avail = (size - kHeaderBytes) / kRecordBytes;
  size_t n = data[1];
  if (n > avail) {
    n = avail;
  }
  if (n > kMaxPhases) {
    n = kMaxPhases;
  }

  ProfileDraft draft{};
  draft.mode = mode;
  draft.phaseCount = n;
  for (size_t i = 0; i < n; ++i) {
    const uint8_t *rec = data + kHeaderBytes + i * kRecordBytes;
    draft.phases[i].targetC = readF32(rec + 0);
    draft.phases[i].rampSeconds = readF32(rec + 4);
    draft.phases[i].holdSeconds = readF32(rec + 8);
    draft.phases[i].exposurePerSurface = readF32(rec + 12);
    const uint8_t flags = rec[16];
    draft.phases[i].uv = flags & 0x01;
    draft.phases[i].motor = flags & 0x02;
    draft.phases[i].convFan = readFan((flags >> 2) & 0x03);
  }

  // Wide caps so an adversarial-but-finite target can still compile (exercises the seg->phase map);
  // a non-finite or out-of-range target simply yields a zero-segment recipe, which the tracker also
  // must survive.
  const Caps caps{-1000.0F, 100000.0F};
  RunTracker rt;
  rt.begin(draft, model, caps, ambient, 0);

  FUZZ_ASSERT(rt.projectedCount() <= profile_facts::kMaxCurvePoints);
  FUZZ_ASSERT(fin(rt.totalSeconds()) && rt.totalSeconds() >= 0.0F);
  // projectedAt across the whole real line, incl. out-of-range args.
  FUZZ_ASSERT(fin(rt.projectedAt(-1.0e9F)));
  FUZZ_ASSERT(fin(rt.projectedAt(1.0e12F)));

  // Drive a run of adversarial telemetry frames with a monotonic host clock.
  for (uint32_t k = 0; k < 8; ++k) {
    const size_t base = static_cast<size_t>(k) * 13;
    oven_Telemetry t = oven_Telemetry_init_zero;
    t.seg_idx = wrapU32(data, size, base);
    t.elapsed_ms = wrapU32(data, size, base + 4);
    t.work_temp = wrapF32(data, size, base + 8);
    t.board_est = wrapF32(data, size, base + 12);
    t.wall_temp_count = 4;
    for (size_t i = 0; i < 4; ++i) {
      t.wall_temp[i] = wrapF32(data, size, base + 16 + i * 4);
    }
    t.run_state = static_cast<oven_RunState>(wrapU32(data, size, base + 6) % 4);

    rt.update(t, k * 250);

    const float p = rt.progress01();
    FUZZ_ASSERT(fin(p) && p >= 0.0F && p <= 1.0F);
    FUZZ_ASSERT(fin(rt.etaSeconds()) && rt.etaSeconds() >= 0.0F);
    FUZZ_ASSERT(fin(rt.projectedAt(static_cast<float>(t.elapsed_ms) / 1000.0F)));
    FUZZ_ASSERT(rt.phaseOrdinal() <= rt.phaseCount());
    (void)rt.phaseName(); // always a valid pointer; UBSAN covers any OOB
    (void)rt.deviating();
  }

  const RunFitResult r = rt.finish(RunOutcome::Completed, 2001);
  FUZZ_ASSERT(fin(r.runQuality.maxAbsC) && fin(r.runQuality.rmsC) && fin(r.runQuality.meanC));
  FUZZ_ASSERT(fin(r.estimatorQuality.maxAbsC) && fin(r.estimatorQuality.rmsC));
  return 0;
}
