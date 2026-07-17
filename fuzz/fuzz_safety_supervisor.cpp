// Internal-property harness: the controller's output safety gate (A4b,
// lib/control_logic/safety_supervisor.h) must uphold its L3 safety invariants under an
// arbitrary (raw, pre-validation) recipe AND an adversarial driver sequence — arbitrary
// high-limit temperatures (incl. NaN/Inf), commanded duties, clock jumps, and
// authorize / trip / clear / disarm events.
//
// Like fuzz_executor this is an invariant harness (no oracle): it drives a real
// SafetySupervisor over the same object graph the unit test builds (a real ControllerLink
// gate + FakeClock + fakes for the switch/contactor/thermocouples) and asserts, every tick:
//   - Latch monotonicity: nothing but clearFault() ever clears the fault latch, so across a
//     tick() (which can only trip) faulted() never goes true→false.
//   - Trip ⇒ safe: a faulted supervisor has the contactor open, and reports a non-NONE code.
//   - Run-blind refusal: armed + authorized with NO usable high-limit channel must be faulted
//     after the tick (SENSOR_FAULT, unless an earlier L3 check already tripped).
//   - clampSetpoint totality: for any probe (NaN/±Inf/huge/negative) the result is finite and
//     within [0, REFLOW_HARD_MAX_C] — the widest possible ceiling.
// Plus the implicit UBSan/ASan check that armRun over a raw recipe never overflows the
// Σ dur_ms budget or walks segments_count out of bounds.
//
// Input format (little-endian; seed_gen.cpp emits matching seeds):
//   [0]      flags (reserved)
//   [1]      requested segment count (clamped to the bytes available and to 32)
//   then N × 5-byte segment records (only the fields armRun reads):
//     [0..3]  dur_ms  : uint32 (raw → exercises the Σ-overflow / saturation path)
//     [4]     channels: bit0 uv, bit1 motor (drive deriveMode's cure/reflow cap selection)
//   then the driver — 11-byte records, one per tick until the bytes run out:
//     [0..3]  wall temp   : float32 (raw; applied to every wall channel)
//     [4..7]  duty        : float32 (raw; commanded to the heater actuator)
//     [8..9]  clock step  : uint16 ms to advance (0..65535 — two steps clear the stuck window)
//     [10]    control     : bit0 refresh-auth, bit1 trip, bit2 clearFault, bit3 disarm,
//                           bit4 fault every wall channel (no usable high-limit)
#include <cmath>
#include <cstring>

#include "fuzz_util.h"

#include "controller_link.h"
#include "frame_link.h"
#include "handshake.h"
#include "heater_actuator.h"
#include "helpers/fake_clock.h"
#include "helpers/fake_contactor.h"
#include "helpers/fake_heater_switch.h"
#include "helpers/fake_thermocouples.h"
#include "helpers/pipe_transport.h"
#include "message_router.h"
#include "messages.h"
#include "oven.pb.h"
#include "oven_safety.h"
#include "safety_supervisor.h"
#include "schema.h"

namespace {

constexpr size_t kHeaderBytes = 2;
constexpr size_t kSegRecord = 5;
constexpr size_t kDriverRecord = 11;
constexpr pb_size_t kMaxSeg = 32; // oven.Recipe.segments max_count
constexpr int kTickCap = 100000;  // driver bytes bound the real work; this is a runaway backstop

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

oven_Heartbeat makeHb(uint32_t session, uint32_t seq, bool enable) {
  oven_Heartbeat h = oven_Heartbeat_init_default;
  h.session = session;
  h.seq = seq;
  h.enable = enable;
  return h;
}

// clampSetpoint must be total; probe it with the values most likely to break a naive clamp.
void assertClampTotal(const SafetySupervisor &sup) {
  const float probes[] = {std::nanf(""),
                          std::numeric_limits<float>::infinity(),
                          -std::numeric_limits<float>::infinity(),
                          -5.0f,
                          1e9f,
                          50.0f,
                          0.0f};
  for (float p : probes) {
    const float r = sup.clampSetpoint(p);
    FUZZ_ASSERT(std::isfinite(r));
    FUZZ_ASSERT(r >= 0.0f);
    FUZZ_ASSERT(r <= oven_safety::REFLOW_HARD_MAX_C + 0.001f);
  }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size < kHeaderBytes) {
    return 0;
  }

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
  for (pb_size_t i = 0; i < n; ++i) {
    const uint8_t *rec = data + kHeaderBytes + i * kSegRecord;
    oven_Segment &s = recipe.segments[i];
    s.dur_ms = readU32(rec); // raw: drives the Σ-overflow / uint32 saturation path
    s.heat_c = 100.0f;       // L3 arming doesn't read heat_c; keep it finite
    const uint8_t ch = rec[4];
    s.uv = (ch & 0x01) != 0; // uv / motor pick the cap via deriveMode (content, not tag)
    s.motor = (ch & 0x02) != 0;
  }

  // The unit-test object graph: a real ControllerLink gate over a loopback, with fakes for
  // every output and the high-limit sensor.
  LoopbackPipe pipe;
  protocol::IMessageObserver sink;
  protocol::MessageRouter router(sink);
  FakeClock clk;
  protocol::FrameLink link(pipe.a(), TF_MASTER, router);
  ControllerLink ctrl(link, clk);
  FakeHeaterSwitch heaterSw;
  FakeContactor contactor;
  FakeThermocouples tc;
  HeaterActuator heater(heaterSw, clk);
  SafetySupervisor sup(ctrl, heater, contactor, tc, clk);

  // Authorize a session up front; the driver keeps it fresh (bit0) or lets it go stale.
  const uint32_t kSession = 0x1234;
  ctrl.handshake().onPeerHello([] {
    oven_Hello h = oven_Hello_init_default;
    h.proto_ver = protocol::kProtoVer;
    h.schema_hash = protocol::kSchemaHash;
    return h;
  }());
  ctrl.gate().adoptSession(kSession);
  ctrl.gate().onHeartbeat(makeHb(kSession, 0, true));

  sup.armRun(recipe); // raw recipe — Σ dur_ms must not overflow (ASan/UBSan watch this)
  bool armed = true;
  uint32_t hbSeq = 1;

  const uint8_t *drv = data + kHeaderBytes + static_cast<size_t>(n) * kSegRecord;
  const size_t drvBytes = size - (kHeaderBytes + static_cast<size_t>(n) * kSegRecord);
  const size_t drvCount = drvBytes / kDriverRecord;

  const int ticks =
      drvCount < static_cast<size_t>(kTickCap) ? static_cast<int>(drvCount) : kTickCap;
  for (int i = 0; i < ticks; ++i) {
    const uint8_t *r = drv + static_cast<size_t>(i) * kDriverRecord;
    const float wallTemp = readF32(r);
    const float duty = readF32(r + 4);
    const uint32_t step = readU16(r + 8);
    const uint8_t ctl = r[10];

    // Controls first, so their effect is visible to this tick.
    if (ctl & 0x01) {
      ctrl.gate().onHeartbeat(makeHb(kSession, hbSeq++, true)); // keep authorization fresh
    }
    if (ctl & 0x02) {
      sup.trip(oven_FaultCode_FAULT_INTERNAL);
    }
    if (ctl & 0x04) {
      sup.clearFault();
    }
    if (ctl & 0x08) {
      sup.disarmRun();
      armed = false;
    }

    const bool allWallsFaulted = (ctl & 0x10) != 0;
    for (int w = 0; w < FakeThermocouples::kWalls; ++w) {
      tc.wallC[w] = wallTemp;
      tc.wallFault[w] = allWallsFaulted;
    }
    heater.setDuty(duty); // duty() returns the clamped commanded value the stuck-check reads

    clk.advance(step);

    const bool faultedBefore = sup.faulted();
    const bool clearedThisTick = (ctl & 0x04) != 0;
    const bool authorizedBeforeTick = ctrl.authorized();

    sup.tick();

    // Latch monotonicity: only clearFault() clears the latch, and tick() only ever trips.
    if (!clearedThisTick) {
      FUZZ_ASSERT(!(faultedBefore && !sup.faulted()));
    }
    // A faulted supervisor is safe (mains isolated) and names a real code.
    if (sup.faulted()) {
      FUZZ_ASSERT(sup.safe());
      FUZZ_ASSERT(!contactor.closed);
      FUZZ_ASSERT(sup.faultCode() != oven_FaultCode_FAULT_NONE);
    }
    // Run-blind refusal: armed + authorized with no usable high-limit channel must be faulted.
    if (armed && authorizedBeforeTick && allWallsFaulted) {
      FUZZ_ASSERT(sup.faulted());
    }

    assertClampTotal(sup);
  }

  return 0;
}
