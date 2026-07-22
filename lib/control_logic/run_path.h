// ControllerRunPath — the controller's closed-loop run composition (design.md §5/§11, backlog
// A10; the wiring A5/A6 left for "D4's real temp source").
//
// The executor (A6), PID (A5) and safety supervisor (A4a/A4b) were each built and host-tested in
// isolation; nothing ever composed them into the actual per-loop control sequence — main.cpp wired
// only the link + supervisor + a StubThermocouples. This class is that missing composition, in one
// place so main.cpp (the CONTROL_SIM build) and the host closed-loop test drive the *identical*
// sequence and cannot drift (§11). D4 reuses it verbatim once real thermocouples exist — the only
// change there is which IThermocouples is injected.
//
// Per loop, tick() does the sequence design.md §5/§11 specifies:
//   1. read the mode's control sensor (workpiece in reflow, wall in cure — §5),
//   2. advance the executor with that measurement,
//   3. while a run is live: clamp the executor's setpoint (SafetySupervisor, defence-in-depth),
//      compute the feedforward holding duty (thermal_math.h steadyStateDuty), run the PID, and
//      command the resulting duty onto the actuator,
//   4. arm/disarm the supervisor's L3 checks around the run's RUNNING lifetime.
// It does NOT own the run lifecycle: ControllerLink still load()/start()/abort()s the executor off
// the accepted-setup path. SafetySupervisor::tick() still runs LAST in the loop and has the final
// word over the duty this commands (§4) — this class only ever *requests* a duty.
//
// The executor stays mode-agnostic (§5/A6): the workpiece-vs-wall choice lives here, derived from
// recipe *content* (the same deriveMode rule the safety cap selector uses — never the untrusted
// tag). Header-only; all collaborators injected by reference, must outlive this object.
#pragma once

#include <cmath> // std::isfinite

#include "IClock.h"
#include "IDoorSensor.h"
#include "IThermocouples.h"
#include "controller_link.h"
#include "heater_actuator.h"
#include "heater_control.h"
#include "link_params.h" // kCommandTimeoutMs (the orphan window is pinned against it)
#include "oven.pb.h"
#include "oven_safety.h" // deriveMode
#include "profile_executor.h"
#include "safety_supervisor.h"
#include "setpoint_shaper.h" // the reference trajectory the PI tracks (§5 overshoot fix)
#include "thermal_math.h"    // OvenModel, rampFeedforwardDuty (feedforward)

// The shaper's approach floor must stay above the executor's stall floor, or a run that is arriving
// normally — just slowly, by design — gets faulted TARGET_UNREACHABLE by its own watchdog. The two
// constants live in different headers for good reasons (the executor knows nothing of the plant
// model); this is the one place that includes both, so it is where the relationship is pinned.
static_assert(SetpointShaper::Config{}.minApproachRateCPerS >
                  2.0f * ProfileExecutor::Config{}.rateFloorCPerS,
              "setpoint taper must stay clear of the executor's measured-rate stall floor");

class ControllerRunPath {
public:
  // How long a run may stay RUNNING with the peer un-authorized before the controller ends it to
  // IDLE (A11). Must be >> kCommandTimeoutMs (750 ms), so a dropped frame or a brief cable glitch —
  // which a SAME-session reconnect legitimately resumes from — never ends a run. ~30 s placeholder,
  // **TBD §10** (the "orphan timeout" open, tuned with the other link windows).
  static constexpr uint32_t kOrphanTimeoutMs = 30000;

  ControllerRunPath(ProfileExecutor &exec, HeaterControl &pid, SetpointShaper &shaper,
                    SafetySupervisor &safety, HeaterActuator &heater, ControllerLink &link,
                    IThermocouples &tc, IDoorSensor &door, IClock &clock, const OvenModel &model)
      : exec_(exec), pid_(pid), shaper_(shaper), safety_(safety), heater_(heater), link_(link),
        tc_(tc), door_(door), clock_(clock), model_(model) {}

  // Advance one control loop. Call before heater_.tick() and safety_.tick().
  void tick() {
    // 0. Door open (§15, DECIDED): "the controller autonomously safes and ends the run to idle —
    //    same for both modes, no pause state, no resume logic, no context retained (it stays a
    //    stateless profile executor)". All resume intelligence is the CYD's.
    //
    //    Deliberately NOT a fault. §22 is explicit that a door-open during a run is an EXPECTED
    //    event, not a red alarm — routing it through SafetySupervisor::trip() would latch a modal
    //    that demands an acknowledge, and would keep the fault sticky after the door shut again.
    //    The heat is already gone either way: DS1 removed element power in hardware before this
    //    line ran (§4 L0). Ending the run is the *bookkeeping*, not the mitigation.
    //
    //    Checked first, so the abort lands before this tick computes a duty: the executor goes
    //    IDLE, its Output.safe becomes true, and step 3 below commands OFF on the same iteration.
    door_open_ = door_.isOpen();
    if (door_open_ && exec_.state() == oven_RunState_RUN_STATE_RUNNING) {
      exec_.abort();
      door_aborted_ = true;
    }

    // 0b. Orphaned run (A11, §9/§15): the peer (CYD) vanished or rebooted while a run is live.
    //     SafetySupervisor already cut the outputs the moment link_.authorized() dropped — but
    //     nothing ENDS the run, so the executor keeps advancing (duty 0) and telemetry keeps
    //     advertising RUNNING. A freshly booted CYD then renders a phantom run nobody started
    //     (bench 2026-07-21). End it to IDLE, same posture as the door above: bookkeeping, not
    //     mitigation — deliberately NOT a fault (§22 excludes it) and NOT a
    //     SafetySupervisor::trip().
    //
    //     Two triggers. A changed peer boot_nonce is positive evidence the operator's context is
    //     gone — a rebooted CYD draws a fresh esp_random() session it can never re-reach — so end
    //     immediately. Sustained non-authorization past orphanTimeoutMs covers cable-pulled /
    //     CYD-unpowered, where no new nonce ever arrives; the window is >> kCommandTimeoutMs so a
    //     dropped frame or a brief glitch (which a SAME-session reconnect resumes from) never ends
    //     a run. Checked before the duty math so an ended run commands OFF this same tick.
    if (exec_.state() == oven_RunState_RUN_STATE_RUNNING) {
      const uint32_t peer_nonce = link_.handshake().peer().boot_nonce;
      if (!orphan_tracking_) {
        // Run just became live: snapshot the peer we started with and reset the grace timer. (On
        // the first RUNNING tick the executor was flipped by ControllerLink::start() during
        // service(), so this fires here rather than off the arm latch below.)
        orphan_tracking_ = true;
        run_peer_nonce_ = peer_nonce;
        unauth_counting_ = false;
      }
      const bool rebooted = link_.handshake().sawPeer() && peer_nonce != run_peer_nonce_;
      bool timed_out = false;
      if (link_.authorized()) {
        unauth_counting_ = false; // a same-session reconnect clears the window: the run continues
      } else if (!unauth_counting_) {
        unauth_counting_ = true;
        unauth_since_ms_ = clock_.millis();
      } else if (static_cast<uint32_t>(clock_.millis() - unauth_since_ms_) >= kOrphanTimeoutMs) {
        timed_out = true;
      }
      if (rebooted || timed_out) {
        exec_.abort();
        link_.gate().clearSession(); // return the gate to IDLE-safe; a stale session authorizes
                                     // nothing, and a returning/rebooted CYD re-Starts a new one
        orphan_aborted_ = true;
      }
    } else {
      orphan_tracking_ = false;
    }

    // 1. The mode's control sensor. Cure controls on raw wall temp (a good air proxy at 80 °C),
    //    reflow on the workpiece probe (§5/§6). deriveMode keys off recipe content, not the tag.
    const bool cure =
        link_.hasRecipe() && oven_safety::deriveMode(link_.acceptedRecipe()) == oven_Mode_MODE_CURE;
    const TcReading r = cure ? tc_.wall(0) : tc_.workpiece();
    const bool valid = !r.fault && std::isfinite(r.celsius);

    // 2. Advance the executor. A faulted control channel makes it fault SENSOR_FAULT (refuse to
    //    run blind, §4); an idle executor ignores the reading.
    exec_.tick(r.celsius, valid);

    // 4. (done before the duty math so a fresh run's L3 checks are armed this same tick). Arm on
    //    entering RUNNING, disarm on leaving it — tracked by our own latch rather than a state
    //    edge, because ControllerLink::start() flips the executor to RUNNING during service(),
    //    before this tick even samples it. armRun stays RUNNING through the backup cooldown, which
    //    is correct: the over-temp/high-limit checks matter while the chamber is still hot.
    const bool running = exec_.state() == oven_RunState_RUN_STATE_RUNNING;
    if (running && !armed_) {
      if (link_.hasRecipe()) {
        safety_.armRun(link_.acceptedRecipe());
      }
      armed_ = true;
    } else if (!running && armed_) {
      safety_.disarmRun();
      armed_ = false;
    }

    // Route an executor fault into the supervisor's latch (A4b: "the executor's Output.fault routes
    // in via trip(code)"). Without this a SENSOR_FAULT/TARGET_UNREACHABLE would stop the run but
    // leave the contactor closed — the supervisor only self-trips on its own L3 checks.
    const ProfileExecutor::Output &o = exec_.output();
    if (o.fault != oven_FaultCode_FAULT_NONE && !safety_.faulted()) {
      safety_.trip(o.fault);
    }

    // 3. Drive the heater. When the executor wants outputs off (idle / done / fault / cooldown),
    //    command OFF and reset the PID *and the shaper* so neither integral windup nor a stale
    //    reference leaks across a stop (§5). Otherwise: shape the setpoint into a trajectory the
    //    plant can actually follow, feed forward along it, and let the PI trim the residual.
    if (o.safe) {
      pid_.reset();
      shaper_.reset();
      heater_.setDuty(0.0f);
      return;
    }
    // The executor's setpoint, safety-clamped, is the TARGET; the shaper turns it into the moving
    // reference the loop tracks (§5). On a RAMP_ASAP that is the difference between a step the PI
    // answers with saturated duty — overcharging the element, which then carries the chamber ~15 °C
    // past setpoint (bench, 2026-07-19) — and a paced approach that eases duty off before arrival.
    // Transparent for a timed ramp, whose setpoint already moves slower than the plant can follow.
    const float sp = safety_.clampSetpoint(o.setpointC);
    const SetpointShaper::Shaped s = shaper_.update(sp, r.celsius, o.convFan, model_);
    // Feedforward along the trajectory: the holding duty at where we are GOING plus the duty that
    // buys the reference's climb rate (DutyModel::rateGain — the term B2 added for exactly this and
    // that nothing consumed until now). Evaluated at the shaped setpoint, not the measurement: the
    // point of feedforward is to command what the trajectory needs, leaving feedback the residual.
    const float ff = rampFeedforwardDuty(model_, s.setpointC, s.ratePerS, o.convFan);
    // Feedforward owns the ramp, the integrator owns the hold (see HeaterControl::setIntegrating).
    // The shaper reports a rate of exactly 0 once the reference has arrived, which is precisely the
    // "nothing is moving, so accumulated error means a standing offset" condition — including the
    // reflow hold-entry gate, where the integrator SHOULD drive the chamber above setpoint to pull
    // a lagging workpiece up.
    pid_.setIntegrating(s.ratePerS == 0.0f);
    const float duty = pid_.update(s.setpointC, r.celsius, ff);
    heater_.setDuty(duty);
  }

  const ProfileExecutor::Output &output() const { return exec_.output(); }
  bool armed() const { return armed_; }
  // The reference the PI is tracking this tick (§5). Diagnostic only — telemetry deliberately
  // reports the EXECUTOR's setpoint (see src_control/main.cpp), because §15's chart compares the
  // run against the CYD's projection of the authored profile, not against our internal trajectory.
  float shapedSetpointC() const { return shaper_.reference(); }

  // Door state as of the last tick() — what telemetry reports (§9's `doorOpen` bit) so the CYD can
  // gate Start (§19), wake the display (§17) and say why a run ended (§15).
  bool doorOpen() const { return door_open_; }
  // Latched: did a door-open end a run? Cleared by clearDoorAbort(), which the caller does once it
  // has reported the edge. Purely informational — nothing safety-critical reads it.
  bool doorAborted() const { return door_aborted_; }
  void clearDoorAbort() { door_aborted_ = false; }

  // Latched: did the peer being lost/rebooted end a run (A11)? Mirrors doorAborted() —
  // informational, read by main.cpp to push an immediate telemetry frame so a fresh CYD sees IDLE
  // promptly rather than a phantom RUNNING. Cleared by the caller once it has reported the edge.
  bool orphanAborted() const { return orphan_aborted_; }
  void clearOrphanAbort() { orphan_aborted_ = false; }

private:
  ProfileExecutor &exec_;
  HeaterControl &pid_;
  SetpointShaper &shaper_;
  SafetySupervisor &safety_;
  HeaterActuator &heater_;
  ControllerLink &link_;
  IThermocouples &tc_;
  IDoorSensor &door_;
  IClock &clock_;
  const OvenModel &model_;
  bool armed_ = false; // whether we have armed the supervisor for the current RUNNING run
  bool door_open_ = false;
  bool door_aborted_ = false;
  // A11 orphan-abort tracking (all scoped to the current RUNNING run).
  uint32_t run_peer_nonce_ = 0;  // the peer boot_nonce this run started against
  bool orphan_tracking_ = false; // have we snapshotted the run's peer/timer yet
  bool unauth_counting_ = false; // are we timing a sustained loss of authorization
  uint32_t unauth_since_ms_ = 0; // when authorization was first lost (clock_.millis())
  bool orphan_aborted_ = false;  // latched edge for telemetry
};

// The orphan-abort grace window (A11) must sit far above the heartbeat command-timeout, or a normal
// dropped-frame gap — the very thing a same-session reconnect resumes from — would end runs. This
// is the one place that includes both link_params.h and this Config, so the relationship is pinned
// here (mirroring the shaper/executor static_assert above).
static_assert(ControllerRunPath::kOrphanTimeoutMs > 10 * protocol::kCommandTimeoutMs,
              "orphan timeout must be much larger than the heartbeat command-timeout");
