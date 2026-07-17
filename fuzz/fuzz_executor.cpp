// Internal-property harness: the profile executor (A6, lib/control_logic/profile_executor.h)
// must obey its safety invariants under an arbitrary recipe AND an adversarial temperature
// trajectory — and, above all, it must always *terminate*.
//
// The headline property is **liveness**: the per-segment watchdog exists precisely so an
// unreachable target cannot hang the run at full duty (design.md §5). So no matter what
// segments and what measured-temperature trajectory the fuzzer supplies, the executor must
// leave RUNNING (reach DONE or FAULT) within a bounded number of ticks — never spin forever.
// A hand-written test can only try a handful of trajectories; coverage-guided mutation pushes
// at the k×dur / rate-floor / stall-cap boundaries with NaN/Inf temps and out-of-range interp.
//
// Unlike fuzz_compiler's differential, this is an invariant harness (no oracle): it drives a
// real ProfileExecutor over a FakeClock and asserts postconditions every tick. It fuzzes a
// *raw* recipe (arbitrary segments, pre-validation) on purpose — the executor is the last thing
// standing if a bad recipe ever reaches it, so it must stay robust without leaning on A7.
//
// Input format (documented so seed_gen.cpp can emit an on-contract seed; little-endian):
//   [0]      flags : bit0 → holdEntryGated (reflow-style measured hold-entry gate)
//   [1]      requested segment count (clamped to the bytes available and to 32)
//   then N × 10-byte segment records:
//     [0]     interp (taken mod 5 → includes one out-of-range value, the default→HOLD path)
//     [1..4]  heat_c  : float32 (raw bytes → NaN/Inf/denormal)
//     [5..8]  dur_ms  : uint32  (clamped to 0..300000 so an honestly-long hold can't be
//                                mistaken for a hang; the watchdog is what this hunts)
//     [9]     channels: bit0 uv, bit1 motor, bit2 conv_fan (bit3 reserved — no cool fan, §6)
//   then the trajectory — 6-byte records, consumed one per tick until the bytes run out:
//     [0..3]  control temp : float32 (raw; a non-finite value drives the controlValid=false path)
//     [4..5]  clock step   : uint16; the tick advances the clock by (1000 + value) ms, so time
//                            always moves forward and every bounded wait must resolve.
#include <cmath>
#include <cstring>

#include "fuzz_util.h"

#include "helpers/fake_clock.h" // -I test (fuzz_common); reuse the same fake the unit tests use
#include "profile_executor.h"

namespace {

constexpr size_t kHeaderBytes = 2;
constexpr size_t kSegRecord = 10;
constexpr size_t kTrajRecord = 6;
constexpr pb_size_t kMaxSeg = 32; // oven.Recipe.segments max_count
// Comfortably above the worst-case tick count: 32 non-gated segments of ≤300 s at ≥1 s/tick
// (~10k ticks), or ≤32 gated waits each capped at maxWaitMs=1.2M ms / 1 s (~38k ticks). If the
// executor is still RUNNING after this many ticks, liveness is broken.
constexpr int kTickCap = 200000;

float readF32(const uint8_t *p) {
  float f;
  std::memcpy(&f, p, sizeof(f));
  return f;
}

uint32_t readU32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t readU16(const uint8_t *p) {
  return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8));
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size < kHeaderBytes) {
    return 0;
  }

  const bool gated = (data[0] & 1) != 0;

  const size_t segsAvailable = (size - kHeaderBytes) / kSegRecord;
  pb_size_t n = data[1];
  if (n > kMaxSeg) {
    n = kMaxSeg;
  }
  if (n > segsAvailable) {
    n = static_cast<pb_size_t>(segsAvailable);
  }

  oven_Recipe recipe = oven_Recipe_init_zero;
  recipe.id = 1;
  recipe.seq = 1;
  recipe.segments_count = n;
  float expectedHi = 0.0f; // the executor clamps its setpoint to [0, max finite target]
  for (pb_size_t i = 0; i < n; ++i) {
    const uint8_t *rec = data + kHeaderBytes + i * kSegRecord;
    oven_Segment &s = recipe.segments[i];
    s.interp = static_cast<oven_Interp>(rec[0] % 5); // 0..4: 4 is out of range on purpose
    s.heat_c = readF32(rec + 1);
    s.dur_ms = readU32(rec + 5) % 300001u; // 0..300000 ms
    const uint8_t ch = rec[9];
    s.uv = (ch & 0x01) != 0;
    s.motor = (ch & 0x02) != 0;
    s.conv_fan = (ch & 0x04) != 0;
    if (std::isfinite(s.heat_c) && s.heat_c > expectedHi) {
      expectedHi = s.heat_c;
    }
  }

  FakeClock clk;
  ProfileExecutor exec(clk);
  exec.load(recipe, gated);
  exec.start();

  const uint8_t *traj = data + kHeaderBytes + static_cast<size_t>(n) * kSegRecord;
  const size_t trajBytes = size - (kHeaderBytes + static_cast<size_t>(n) * kSegRecord);
  const size_t trajCount = trajBytes / kTrajRecord;

  uint32_t lastSegIdx = 0;
  for (int i = 0; i < kTickCap && exec.state() == oven_RunState_RUN_STATE_RUNNING; ++i) {
    float temp = 0.0f;
    uint32_t step = 1000; // floor: time always advances, so bounded waits must resolve
    if (static_cast<size_t>(i) < trajCount) {
      const uint8_t *r = traj + static_cast<size_t>(i) * kTrajRecord;
      temp = readF32(r);
      step = 1000u + readU16(r + 4);
    }
    clk.advance(step);
    exec.tick(temp, std::isfinite(temp));

    const ProfileExecutor::Output &o = exec.output();

    // Setpoint is always finite and within [0, max finite target].
    FUZZ_ASSERT(std::isfinite(o.setpointC));
    FUZZ_ASSERT(o.setpointC >= -0.001f);
    FUZZ_ASSERT(o.setpointC <= expectedHi + 0.001f);

    // Segment index stays in range and never runs backward while executing.
    if (o.runState == oven_RunState_RUN_STATE_RUNNING) {
      FUZZ_ASSERT(o.segIdx < recipe.segments_count);
      FUZZ_ASSERT(o.segIdx >= lastSegIdx);
      lastSegIdx = o.segIdx;
      FUZZ_ASSERT(!o.safe); // running => outputs live
    } else {
      // Any non-running terminal state is safe, and a fault code implies FAULT.
      FUZZ_ASSERT(o.safe);
      if (o.fault != oven_FaultCode_FAULT_NONE) {
        FUZZ_ASSERT(o.runState == oven_RunState_RUN_STATE_FAULT);
      }
    }
  }

  // Liveness: the run must have left RUNNING within the tick cap.
  FUZZ_ASSERT(exec.state() != oven_RunState_RUN_STATE_RUNNING);
  return 0;
}
