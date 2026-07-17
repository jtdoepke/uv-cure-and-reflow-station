// ProfileExecutor — sequences a validated recipe into the moving setpoint the heater
// control loop tracks (design.md §5, backlog A6).
//
// This is the controller's run engine. Given an accepted oven_Recipe, it walks the
// segments and, every tick, emits {setpoint, resolved channel states, seg index, run
// state}. A5's PID *tracks* the setpoint; A6 only *emits* it, so the two are built and
// tested independently (§5). The executor never touches an output directly — it
// produces an Output struct the caller routes to the PID / channel drivers, and its
// `safe`/`fault` signal is the seam into SafetySupervisor::trip() (A4a left it for
// "the A6/A7 fault paths").
//
// Interp semantics (§5, proto oven_Interp):
//   RAMP_OVER_TIME  — sweep the setpoint linearly from the previous target to heat_c
//                     across dur_ms.
//   RAMP_ASAP       — command heat_c (A5 saturates duty) and advance when the measured
//                     control temp reaches it (target-gated). dur_ms is the projected
//                     reach time B1 baked in for the ETA/watchdog (proto: "dur_ms is
//                     the stall watchdog"), so the executor watchdogs against the SAME
//                     projection the CYD showed the operator — no OvenModel needed here.
//   HOLD            — hold heat_c for dur_ms. In reflow (holdEntryGated) the hold timer
//                     does not start until the measured temp is within a band of target
//                     (§5 hold-entry gate); cure holds are dose timers and start at once.
//
// Per-segment watchdog (§5): every target-gated wait (RAMP_ASAP, gated hold-entry)
// faults to TARGET_UNREACHABLE when it outruns k× its projected duration, when the
// measured heat rate stays below a floor across a window, or when it exceeds the
// absolute stall cap — so a degraded element faults within minutes instead of baking at
// full duty. The three constants are §10-open ("Per-segment stall policy", §8 step 4);
// they live in Config as clearly-flagged placeholders, tuned against real runs later.
//
// Mode-agnostic by construction (§5): the caller selects the control sensor (workpiece
// in reflow, wall in cure) and passes the hold-entry policy at load(); the executor
// itself never branches on mode. It is also robust to a raw (post-nanopb, pre-validated)
// recipe — non-finite heat_c and out-of-range interp are handled, and the emitted
// setpoint is always finite and clamped to the recipe's own maximum target, so it can
// never command above what was authored (which A7 already bounded to the hard-max).
//
// Header-only; depends only on IClock + the generated oven.pb.h. Clock injected by
// reference, must outlive this object. Keep free of <Arduino.h>.
#pragma once

#include <cmath>
#include <cstdint>

#include "IClock.h"
#include "codec.h" // protocol::wireEnum — read the untrusted, wire-sourced interp without UB
#include "oven.pb.h"

class ProfileExecutor {
public:
  // Watchdog / gating tuning. Defaults are §10 placeholders (see header) — deliberately
  // generous so they only catch a genuinely stalled run, not a slow-but-progressing one.
  struct Config {
    float targetBandC = 2.0f;           // measured within this of target counts as "reached"
    float watchdogK = 3.0f;             // TBD §10: fault when elapsed > k × projected dur_ms
    float rateFloorCPerS = 0.05f;       // TBD §10: min measured heat rate during a gated wait
    uint32_t rateFloorWindowMs = 30000; // TBD §10: window the rate must beat the floor over
    uint32_t maxWaitMs = 1200000;       // absolute stall cap on any single wait (20 min backstop)
  };

  // What the executor wants the outputs to do this tick. The caller routes setpointC to
  // A5 and the channel bools to the UV/fan/motor drivers; `safe`/`fault` drive the
  // SafetySupervisor. When not RUNNING, everything reads its safe/off value.
  struct Output {
    float setpointC = 0.0f;
    bool convFan = false;
    bool coolFan = false;
    bool uv = false;
    bool motor = false;
    uint32_t segIdx = 0;
    oven_RunState runState = oven_RunState_RUN_STATE_IDLE;
    oven_FaultCode fault = oven_FaultCode_FAULT_NONE;
    bool safe = true; // true => the executor wants outputs off (idle / done / fault)
  };

  ProfileExecutor(IClock &clock, Config cfg) : clock_(clock), cfg_(cfg) {}

  // Convenience overload with default Config (mirrors HeaterActuator: a default argument
  // would reference Config's member initializers before the class is complete).
  explicit ProfileExecutor(IClock &clock) : ProfileExecutor(clock, Config{}) {}

  // Load an accepted recipe and its hold-entry policy (reflow = gated on measured temp,
  // cure = time-based). Resets to IDLE; call start() to begin.
  void load(const oven_Recipe &recipe, bool holdEntryGated) {
    recipe_ = recipe;
    holdEntryGated_ = holdEntryGated;
    // The setpoint clamp ceiling: the largest finite target in the recipe. A7 already
    // bounded every target to the reviewed hard-max, so clamping here means the executor
    // can never emit a setpoint above the hard-max, whatever the segments contain.
    maxHeatC_ = 0.0f;
    haveMax_ = false;
    for (pb_size_t i = 0; i < recipe_.segments_count; ++i) {
      const float h = recipe_.segments[i].heat_c;
      if (std::isfinite(h) && (!haveMax_ || h > maxHeatC_)) {
        maxHeatC_ = h;
        haveMax_ = true;
      }
    }
    reset();
  }

  // Begin executing from segment 0. A zero-segment recipe completes immediately.
  void start() {
    fault_ = oven_FaultCode_FAULT_NONE;
    setpointC_ = 0.0f;
    segIdx_ = 0;
    entered_ = false;
    state_ = recipe_.segments_count == 0 ? oven_RunState_RUN_STATE_DONE
                                         : oven_RunState_RUN_STATE_RUNNING;
    writeOutput();
  }

  // Stop the run (Abort / door-open / session end): back to a safe IDLE.
  void abort() { reset(); }

  // Advance one control loop. `controlTempC` is the mode's control sensor; `controlValid`
  // is false on a faulted TC — the executor refuses to run blind and faults safe (§4).
  void tick(float controlTempC, bool controlValid) {
    if (state_ == oven_RunState_RUN_STATE_RUNNING) {
      if (!controlValid) {
        enterFault(oven_FaultCode_FAULT_SENSOR_FAULT);
      } else {
        run(controlTempC);
      }
    }
    writeOutput();
  }

  const Output &output() const { return out_; }
  oven_RunState state() const { return state_; }

private:
  void reset() {
    state_ = oven_RunState_RUN_STATE_IDLE;
    fault_ = oven_FaultCode_FAULT_NONE;
    segIdx_ = 0;
    setpointC_ = 0.0f;
    entered_ = false;
    holdStarted_ = false;
    writeOutput();
  }

  void run(float temp) {
    const oven_Segment &seg = recipe_.segments[segIdx_];
    const uint32_t now = clock_.millis();
    if (!entered_) {
      enterSegment(temp, now);
    }
    const uint32_t elapsed = now - segStartMs_; // wrap-safe uint32 subtraction

    // Read interp as its raw wire int32: nanopb stores the decoded varint verbatim, so an
    // untrusted recipe can leave it holding a value no enumerator defines, and loading that
    // AS oven_Interp would be UB (a fuzzer caught exactly this). An unknown code degrades to
    // HOLD via the default arm.
    switch (protocol::wireEnum(seg.interp)) {
    case oven_Interp_INTERP_RAMP_OVER_TIME: {
      const float target = finiteOr(seg.heat_c, rampFromC_);
      if (seg.dur_ms == 0 || elapsed >= seg.dur_ms) {
        setSetpoint(target);
        advance();
        break;
      }
      const float frac = static_cast<float>(elapsed) / static_cast<float>(seg.dur_ms);
      setSetpoint(rampFromC_ + (target - rampFromC_) * frac);
      break;
    }
    case oven_Interp_INTERP_RAMP_ASAP: {
      const float target = finiteOr(seg.heat_c, 0.0f);
      setSetpoint(target);
      if (reached(temp, target)) {
        advance();
      } else if (waitTimedOut(seg, now, temp, /*useProjected=*/true)) {
        enterFault(oven_FaultCode_FAULT_TARGET_UNREACHABLE);
      }
      break;
    }
    case oven_Interp_INTERP_HOLD:
    default: {
      // An out-of-range interp (only reachable from a raw, unvalidated recipe) degrades
      // to HOLD: bounded and safe rather than undefined.
      const float target = finiteOr(seg.heat_c, 0.0f);
      setSetpoint(target);
      if (holdEntryGated_ && !holdStarted_) {
        if (reached(temp, target)) {
          holdStarted_ = true;
          holdStartMs_ = now;
        } else if (waitTimedOut(seg, now, temp, /*useProjected=*/false)) {
          enterFault(oven_FaultCode_FAULT_TARGET_UNREACHABLE);
          break;
        } else {
          break; // still waiting to arrive; hold timer not started
        }
      } else if (!holdStarted_) {
        holdStarted_ = true;
        holdStartMs_ = now;
      }
      const uint32_t held = now - holdStartMs_;
      if (seg.dur_ms == 0 || held >= seg.dur_ms) {
        advance();
      }
      break;
    }
    }
  }

  // Capture per-segment entry state: timing baseline, the ramp origin (previous target,
  // or the measured temp for the first segment), and the rate-floor window baseline.
  void enterSegment(float temp, uint32_t now) {
    entered_ = true;
    holdStarted_ = false;
    segStartMs_ = now;
    rampFromC_ = segIdx_ == 0 ? finiteOr(temp, 0.0f) : setpointC_;
    rateWinMs_ = now;
    rateWinTempC_ = finiteOr(temp, 0.0f);
  }

  void advance() {
    ++segIdx_;
    if (segIdx_ >= recipe_.segments_count) {
      state_ = oven_RunState_RUN_STATE_DONE;
    } else {
      entered_ = false; // re-enter the next segment on the following tick
    }
  }

  void enterFault(oven_FaultCode code) {
    state_ = oven_RunState_RUN_STATE_FAULT;
    fault_ = code;
    setpointC_ = 0.0f;
  }

  // A target-gated wait has run too long: past k× its projected duration (RAMP_ASAP,
  // where dur_ms is B1's reach estimate), past the absolute stall cap, or below the
  // measured-rate floor across a full window. Any one trips TARGET_UNREACHABLE.
  bool waitTimedOut(const oven_Segment &seg, uint32_t now, float temp, bool useProjected) {
    const float elapsed = static_cast<float>(now - segStartMs_);
    if (useProjected && seg.dur_ms > 0 &&
        elapsed > cfg_.watchdogK * static_cast<float>(seg.dur_ms)) {
      return true;
    }
    if (elapsed > static_cast<float>(cfg_.maxWaitMs)) {
      return true;
    }
    if (static_cast<uint32_t>(now - rateWinMs_) >= cfg_.rateFloorWindowMs) {
      const float dtS = static_cast<float>(now - rateWinMs_) / 1000.0f;
      const float dT = finiteOr(temp, rateWinTempC_) - rateWinTempC_;
      if (dtS > 0.0f && dT < cfg_.rateFloorCPerS * dtS) {
        return true; // a whole window went by without beating the floor
      }
      rateWinMs_ = now;
      rateWinTempC_ = finiteOr(temp, rateWinTempC_);
    }
    return false;
  }

  bool reached(float temp, float target) const {
    const float d = temp - target;
    const float mag = d < 0.0f ? -d : d;
    return mag <= cfg_.targetBandC;
  }

  // Store the emitted setpoint, always finite and clamped to [0, recipe max target].
  // The ceiling floors at 0 (a heater setpoint below 0 is meaningless — command off),
  // so a recipe whose targets are all negative simply emits 0.
  void setSetpoint(float v) {
    const float f = finiteOr(v, 0.0f);
    const float hi = haveMax_ && maxHeatC_ > 0.0f ? maxHeatC_ : 0.0f;
    setpointC_ = f < 0.0f ? 0.0f : (f > hi ? hi : f);
  }

  static float finiteOr(float v, float fallback) { return std::isfinite(v) ? v : fallback; }

  void writeOutput() {
    out_.segIdx = segIdx_;
    out_.runState = state_;
    out_.fault = fault_;
    out_.safe = state_ != oven_RunState_RUN_STATE_RUNNING;
    if (state_ == oven_RunState_RUN_STATE_RUNNING) {
      const oven_Segment &s = recipe_.segments[segIdx_];
      out_.setpointC = setpointC_;
      out_.convFan = s.conv_fan;
      out_.coolFan = s.cool_fan;
      out_.uv = s.uv;
      out_.motor = s.motor;
    } else {
      out_.setpointC = 0.0f;
      out_.convFan = false;
      out_.coolFan = false;
      out_.uv = false;
      out_.motor = false;
    }
  }

  IClock &clock_;
  Config cfg_;

  oven_Recipe recipe_ = oven_Recipe_init_zero;
  bool holdEntryGated_ = false;
  float maxHeatC_ = 0.0f;
  bool haveMax_ = false;

  oven_RunState state_ = oven_RunState_RUN_STATE_IDLE;
  oven_FaultCode fault_ = oven_FaultCode_FAULT_NONE;
  uint32_t segIdx_ = 0;
  float setpointC_ = 0.0f;

  bool entered_ = false;
  uint32_t segStartMs_ = 0;
  float rampFromC_ = 0.0f;

  bool holdStarted_ = false;
  uint32_t holdStartMs_ = 0;

  uint32_t rateWinMs_ = 0;    // rate-floor window baseline time
  float rateWinTempC_ = 0.0f; // rate-floor window baseline temperature

  Output out_;
};
