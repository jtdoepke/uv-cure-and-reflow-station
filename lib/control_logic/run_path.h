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

#include "IThermocouples.h"
#include "controller_link.h"
#include "heater_actuator.h"
#include "heater_control.h"
#include "oven.pb.h"
#include "oven_safety.h" // deriveMode
#include "profile_executor.h"
#include "safety_supervisor.h"
#include "thermal_math.h" // OvenModel, steadyStateDuty (feedforward)

class ControllerRunPath {
public:
  ControllerRunPath(ProfileExecutor &exec, HeaterControl &pid, SafetySupervisor &safety,
                    HeaterActuator &heater, ControllerLink &link, IThermocouples &tc,
                    const OvenModel &model)
      : exec_(exec), pid_(pid), safety_(safety), heater_(heater), link_(link), tc_(tc),
        model_(model) {}

  // Advance one control loop. Call before heater_.tick() and safety_.tick().
  void tick() {
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
    //    command OFF and reset the PID so no integral windup leaks across a stop (§5). Otherwise
    //    feedforward + PI feedback on the clamped setpoint.
    if (o.safe) {
      pid_.reset();
      heater_.setDuty(0.0f);
      return;
    }
    const float sp = safety_.clampSetpoint(o.setpointC);
    const float ff = steadyStateDuty(model_, r.celsius, o.convFan);
    const float duty = pid_.update(sp, r.celsius, ff);
    heater_.setDuty(duty);
  }

  const ProfileExecutor::Output &output() const { return exec_.output(); }
  bool armed() const { return armed_; }

private:
  ProfileExecutor &exec_;
  HeaterControl &pid_;
  SafetySupervisor &safety_;
  HeaterActuator &heater_;
  ControllerLink &link_;
  IThermocouples &tc_;
  const OvenModel &model_;
  bool armed_ = false; // whether we have armed the supervisor for the current RUNNING run
};
