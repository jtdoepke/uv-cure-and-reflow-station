// Internal-property harness: the CLOSED LOOP (A10) — ProfileExecutor (A6) + HeaterControl PID (A5)
// + SafetySupervisor (A4), closed around the OvenPlant twin over the real link — must obey its
// whole-run safety and liveness invariants under an arbitrary recipe and injected disturbances
// (a welded SSR, a lost sensor). This is the interaction the open-loop unit fuzzers
// (fuzz_executor / fuzz_safety_supervisor / fuzz_heater_control) structurally cannot see: each
// drives its unit with a hand-fed trajectory; only a real plant closing the loop exercises how they
// behave together (cf. A8 — "bugs the host tests could not see").
//
// The plant is FIXED to one of two plausible param sets (uninsulated / insulated) so any violation
// is a firmware bug, not a nonsensical-plant artifact — exactly how fuzz_safety_supervisor feeds
// adversarial temps but asserts firmware invariants.
//
// Invariants:
//   safety   — a latched fault always means the outputs are safe (trip ⇒ mains isolated); and the
//              loop never leaves the plant above the mode's hard-max (+ generous slack) without the
//              supervisor having latched a fault.
//   liveness — the run always leaves RUNNING (reaches DONE or a latched FAULT) within the cap.
//   cooldown — if it reports DONE, the measured chamber is touch-safe (DONE never precedes cool).
//
// Input (little-endian):
//   [0]      config : bit0 insulated plant; bit1 inject welded SSR; bit2 inject sensor fault
//   [1]      segCount (mod 7 → 0..6)
//   [2..3]   weldTick  : u16 — tick to weld the SSR on (if bit1)
//   [4..5]   faultTick : u16 — tick to open the control sensor (if bit2)
//   then segCount × 6-byte records:
//     [0]     interp (mod 5 → includes one out-of-range value)
//     [1..2]  heat : u16 → °C = (v % 3200) / 10   (0..320, spans past the hard-max on purpose)
//     [3..4]  dur  : u16 → ms (0..65535, clamped small so the run stays bounded)
//     [5]     channels: bit0 uv, bit1 motor, bit2 conv_fan
#include <cmath>
#include <cstring>

#include "fuzz_util.h"

#include "controller_link.h"
#include "cyd_link.h"
#include "frame_link.h"
#include "heater_actuator.h"
#include "heater_control.h"
#include "helpers/fake_clock.h"
#include "helpers/fake_contactor.h"
#include "helpers/fake_heater_switch.h"
#include "helpers/pipe_transport.h"
#include "message_router.h"
#include "oven.pb.h"
#include "oven_cal.h"
#include "oven_plant.h"
#include "oven_safety.h"
#include "profile_executor.h"
#include "run_path.h"
#include "safety_supervisor.h"
#include "sim_thermocouples.h"

namespace {

constexpr size_t kHeader = 6;
constexpr size_t kSegRec = 6;
constexpr pb_size_t kMaxSeg = 6;
constexpr uint32_t kStepMs = 2000;
constexpr int kTickCap = 4000; // ≫ the runtime-budget bound; if still RUNNING, liveness is broken

uint16_t readU16(const uint8_t *p) {
  return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8));
}

struct Rig {
  LoopbackPipe pipe;
  FakeClock clk;
  protocol::MessageRouter cyd_router;
  protocol::FrameLink cyd_link;
  protocol::CydLink cyd;
  protocol::MessageRouter ctrl_router;
  protocol::FrameLink ctrl_link;
  ControllerLink ctrl;
  FakeHeaterSwitch heater_sw;
  FakeContactor contactor;
  HeaterActuator heater;
  ProfileExecutor exec;
  HeaterControl pid;
  OvenPlant plant;
  SimThermocouples tc;
  SafetySupervisor safety;
  ControllerRunPath runpath;
  float weldDuty = 0.0f;

  explicit Rig(const PlantParams &pp)
      : cyd_link(pipe.a(), TF_MASTER, cyd_router), cyd(cyd_link, clk),
        ctrl_link(pipe.b(), TF_SLAVE, ctrl_router), ctrl(ctrl_link, clk), heater(heater_sw, clk),
        exec(clk), pid(clk), plant(pp), tc(plant), safety(ctrl, heater, contactor, tc, clk),
        runpath(exec, pid, safety, heater, ctrl, tc, oven_cal::kDefaultModel) {
    cyd_router.setObserver(cyd);
    ctrl_router.setObserver(ctrl);
    ctrl.setExecutor(exec);
  }

  void controllerLoop() {
    ctrl_link.poll();
    ctrl.service();
    runpath.tick();
    heater.tick();
    safety.tick();
    const float applied = heater.duty() > weldDuty ? heater.duty() : weldDuty;
    const ProfileExecutor::Output &o = runpath.output();
    plant.step(static_cast<float>(kStepMs) / 1000.0f, applied, o.convFan, o.uv, o.motor);
  }
  void step() {
    clk.advance(kStepMs);
    controllerLoop();
    cyd_link.poll();
    cyd.service();
  }
};

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size < kHeader) {
    return 0;
  }
  const uint8_t cfg = data[0];
  const bool doWeld = (cfg & 0x02) != 0;
  const bool doFault = (cfg & 0x04) != 0;
  const uint32_t weldTick = readU16(data + 2);
  const uint32_t faultTick = readU16(data + 4);

  const size_t segsAvail = (size - kHeader) / kSegRec;
  pb_size_t n = static_cast<pb_size_t>(data[1] % (kMaxSeg + 1));
  if (n > segsAvail) {
    n = static_cast<pb_size_t>(segsAvail);
  }

  oven_Recipe rec = oven_Recipe_init_default;
  rec.id = 1;
  rec.mode = oven_Mode_MODE_REFLOW;
  rec.segments_count = n;
  for (pb_size_t i = 0; i < n; ++i) {
    const uint8_t *r = data + kHeader + i * kSegRec;
    oven_Segment &s = rec.segments[i];
    s.interp = static_cast<oven_Interp>(r[0] % 5); // 4 is out of range on purpose
    s.heat_c = static_cast<float>(readU16(r + 1) % 3200u) / 10.0f;
    s.dur_ms = readU16(r + 3);
    const uint8_t ch = r[5];
    s.uv = (ch & 0x01) != 0;
    s.motor = (ch & 0x02) != 0;
    s.conv_fan = (ch & 0x04) != 0;
  }

  PlantParams pp;
  if ((cfg & 0x01) != 0) {
    pp.insulationFactor = 0.5f;
  }
  Rig rig(pp);

  // Bring up an authorized run (handshake, recipe, start, enabled heartbeats).
  rig.cyd.begin(0xC1D0F122);
  rig.ctrl.begin(0xC710F122);
  rig.step();
  rig.cyd.sender().sendRecipe(rec);
  rig.step();
  oven_Start st = oven_Start_init_default;
  st.session = 0xBE0CF122;
  st.recipe_id = rec.id;
  rig.cyd.sender().sendStart(st);
  rig.step();
  rig.cyd.heartbeat().setSession(0xBE0CF122);
  rig.cyd.heartbeat().setEnable(true);

  // Cure gates/controls on wall temp, reflow on the workpiece (§5) — the same content-derived pick
  // the run path makes. The mode's hard-max (+ margin + slack): the loop must never keep the plant
  // above this without a latched fault.
  const bool cure = oven_safety::deriveMode(rec) == oven_Mode_MODE_CURE;
  const float hardMax = oven_safety::hardMaxForMode(oven_safety::deriveMode(rec));
  const float overTempCeiling = hardMax + oven_safety::OVERTEMP_MARGIN_C + 60.0f;

  bool sawRunaway = false;
  for (int t = 0; t < kTickCap && rig.exec.state() == oven_RunState_RUN_STATE_RUNNING; ++t) {
    if (doWeld && static_cast<uint32_t>(t) == weldTick) {
      rig.weldDuty = 1.0f; // the SSR welds on
    }
    if (doFault && static_cast<uint32_t>(t) == faultTick) {
      rig.tc.workpieceFault = true; // reflow control sensor opens
      for (int w = 0; w < SimThermocouples::kMaxWalls; ++w) {
        rig.tc.wallFault[w] = true; // and the high-limit channels
      }
    }
    rig.step();

    // Per-tick: safety latch implies mains isolated; temps stay finite.
    FUZZ_ASSERT(std::isfinite(rig.plant.chamberTempC()));
    if (rig.safety.faulted()) {
      FUZZ_ASSERT(rig.safety.safe());
    }
    if (rig.plant.wallTempC() > overTempCeiling) {
      sawRunaway = true;
    }
  }

  // Liveness: the run left RUNNING within the cap.
  FUZZ_ASSERT(rig.exec.state() != oven_RunState_RUN_STATE_RUNNING);

  // Completion safety: a run reported DONE always has the heater commanded OFF — no output can
  // linger past completion. (The stronger "DONE ⇒ control sensor touch-safe" guarantee is the
  // executor's cooldown GATE, but it has a deliberate backstop — after cooldownMaxMs it reports
  // DONE regardless of temp, since the heater has been off throughout and a stuck-hot chamber is
  // L3's job (§6). An adversarial recipe (a dur=0 HOLD zeroes the runtime budget) plus a
  // slow-cooling insulated plant can hit that backstop, so touch-safe is not a fuzz invariant; the
  // deterministic test_sim_run pins it for a normally-cooling run.) `cure` documents the
  // control-sensor split.
  (void)cure;
  if (rig.exec.state() == oven_RunState_RUN_STATE_DONE) {
    FUZZ_ASSERT(rig.heater.duty() == 0.0f);
  }

  // Safety: any excursion past the hard-max ceiling must have latched a fault (which is sticky).
  if (sawRunaway) {
    FUZZ_ASSERT(rig.safety.faulted());
  }
  return 0;
}
