// run_tracker.h — the CYD's live run-progress model for the Run/Monitor screen (design.md §15;
// backlog C7). It turns the controller's telemetry stream into everything the screen renders and
// the §16 summary needs, keyed on the AUTHORED Phase[] of the running profile:
//
//   - the projected achievable curve + total duration (profile_facts, computed once at begin);
//   - projectedAt(t): the projected temperature at a live instant, for the chart's now-point and as
//     the reference the deviation/run-fit math compares actual against;
//   - which authored phase the run is in ("Soak 2/4"), the progress fraction, and a slipping ETA;
//   - the RunFitAccumulator wiring (deviation channels + per-phase target checks), so the live
//   amber
//     cue (§15) and the Done summary (§16) read the SAME object.
//
// The gotcha it resolves: the run executes a COMPILED oven_Recipe reported by seg_idx, while the
// projection is keyed on the authored Phase[] (each phase compiles to 0-2 segments + the implicit
// cool tail). begin() compiles the draft once to capture the segment->phase map (CompileResult::
// segmentPhase), so seg_idx maps back to the authored phase deterministically.
//
// Robust to adversarial telemetry (a buggy/rebooted controller can emit garbage): seg_idx past the
// segment count, elapsed_ms rollover, NaN/Inf temps — every output stays finite, bounded, and in
// range (the "no NaN reaches lv_line" guarantee profile_facts holds, extended to the live path;
// pinned by fuzz/fuzz_profile_facts.cpp). No LVGL, no Arduino — host-tested under native_logic_cyd.
#pragma once

#include <cstddef>
#include <cstdint>

#include "oven.pb.h" // oven_Telemetry, oven_RunState
#include "profile_draft.h"
#include "profile_facts.h"
#include "recipe_compiler.h"
#include "run_fit.h"
#include "thermal_math.h" // OvenModel, clampf

class RunTracker {
public:
  // Sentinels for "which authored phase" (returned by currentPhaseIndex): the run has not entered a
  // phase yet, or it is in the implicit cool-down tail (which is no authored phase).
  static constexpr size_t kNoPhase = static_cast<size_t>(-1);
  static constexpr size_t kCoolPhase = static_cast<size_t>(-2);

  RunTracker() = default;
  explicit RunTracker(RunFitAccumulator::Config fitCfg) : fit_(fitCfg) {}

  // Arm the tracker for a run over `draft` (the authored working copy the Confirm screen compiled +
  // uploaded). `caps`/`ambientC` must match that compile so the captured segment->phase map matches
  // the recipe the controller is executing; `model` supplies the calibration the projection uses.
  void begin(const ProfileDraft &draft, const OvenModel &model, Caps caps, float ambientC,
             uint32_t nowMs) {
    draft_ = draft;
    model_ = &model;
    mode_ = draft.mode;
    started_ = true;
    segIdx_ = 0;
    elapsedMs_ = 0;
    runState_ = oven_RunState_RUN_STATE_IDLE;
    curPhase_ = kNoPhase;

    // Compile once to capture the segment->phase map (the recipe itself is Confirm's to upload).
    const CompileResult cr =
        compileRecipe(draft.phases, draft.phaseCount, mode_, model, caps, ambientC, 0, 0);
    segCount_ = cr.hardValid ? cr.recipe.segments_count : 0;
    for (size_t i = 0; i < segCount_ && i < kMaxSegments; ++i) {
      segPhase_[i] = cr.segmentPhase[i];
      // Durations too, for holdProgress01(): B6's resume generator needs to know how much of the
      // interrupted phase's soak actually happened, and the compiled timeline is the only place
      // that says. Captured here rather than recompiled later so it cannot drift from the recipe
      // the controller is executing — the same reason the phase map is captured at begin().
      segDurMs_[i] = cr.recipe.segments[i].dur_ms;
    }

    // Projected achievable trajectory + total duration, computed once (they don't change mid-run).
    projN_ = profile_facts::sampleCurve(draft.phases, draft.phaseCount, mode_, model,
                                        /*achievable=*/true, ambientC, proj_,
                                        profile_facts::kMaxCurvePoints);
    total_ = profile_facts::computeFacts(draft.phases, draft.phaseCount, mode_, model, ambientC)
                 .totalSeconds;

    fit_.beginRun(nowMs);
  }

  // Feed one telemetry frame (§15, 4 Hz). Advances the phase model, evaluates the projected curve
  // at the live instant, and drives the run-fit channels. `nowMs` is the CYD's clock
  // (dt-weighting); the telemetry carries the controller's own seg_idx/elapsed.
  void update(const oven_Telemetry &t, uint32_t nowMs) {
    if (!started_) {
      return;
    }
    segIdx_ = t.seg_idx;
    elapsedMs_ = t.elapsed_ms;
    runState_ = t.run_state;

    const size_t phase = phaseForSeg(t.seg_idx);
    // On entering a new authored phase, open a run-fit phase for its target check. The cool tail is
    // not an authored phase (no target to hit), so it opens nothing — finish() closes the last one.
    if (phase != curPhase_ && phase < draft_.phaseCount) {
      const Phase &p = draft_.phases[phase];
      const uint32_t holdMs =
          recipe_compiler::secondsToMs(profile_facts::phaseHoldSeconds(mode_, p, *model_));
      fit_.beginPhase(nowMs, p.targetC, holdMs);
    }
    curPhase_ = phase;

    const float proj = projectedAt(static_cast<float>(elapsedMs_) / 1000.0f);
    // Constrain the telemetry temps before they reach the deviation stats: a faulted TC reads NaN
    // (map to the projection, so it looks on-track rather than spuriously deviating), and an
    // absurd-but-finite value (untrusted telemetry) is clamped to the physical range — otherwise a
    // 1e38 residual overflows to Inf when the RMS squares it (found by fuzz_run_tracker). Same
    // constrain-don't-just-finite-guard idiom as profile_facts.
    const float ctrl = profile_facts::clampT(profile_facts::finiteOr(controlTemp(t), proj));
    const float board = profile_facts::clampT(profile_facts::finiteOr(t.board_est, ctrl));
    fit_.addSample(nowMs, ctrl, proj, board);
  }

  // --- projected curve / interpolation ---------------------------------------------------------

  // The projected achievable temperature at `tSeconds` from start, by linear interpolation over the
  // precomputed polyline. Clamped/finite: t < 0 -> the start point, t past the end -> the last
  // (cool-tail) point, so a lagging run that outruns the projection reads a sensible value.
  float projectedAt(float tSeconds) const {
    if (projN_ == 0) {
      return 0.0f;
    }
    const float t = profile_facts::clampSec(tSeconds);
    if (t <= proj_[0].t) {
      return proj_[0].T;
    }
    for (size_t i = 1; i < projN_; ++i) {
      if (t <= proj_[i].t) {
        const float t0 = proj_[i - 1].t;
        const float dt = proj_[i].t - t0;
        if (dt <= 0.0f) {
          return proj_[i].T; // zero-width slot (an ASAP corner) — take the endpoint
        }
        const float frac = (t - t0) / dt;
        return proj_[i - 1].T + frac * (proj_[i].T - proj_[i - 1].T);
      }
    }
    return proj_[projN_ - 1].T;
  }

  const profile_facts::CurvePoint *projected() const { return proj_; }
  size_t projectedCount() const { return projN_; }
  float totalSeconds() const { return total_; }

  // --- progress / ETA --------------------------------------------------------------------------

  // Fraction of the projected duration elapsed, clamped to [0,1]. A run past its projection sits at
  // 1.0 (behind schedule) rather than overflowing — the honest "we're at/over time" signal.
  float progress01() const {
    if (!(total_ > 0.0f)) {
      return runState_ == oven_RunState_RUN_STATE_DONE ? 1.0f : 0.0f;
    }
    const float p = (static_cast<float>(elapsedMs_) / 1000.0f) / total_;
    return clampf(p, 0.0f, 1.0f);
  }

  // Estimated seconds remaining (projected total minus elapsed, floored at 0). Re-derived each tick
  // from the live elapsed, so it slips honestly when the oven lags (§15) — an estimate that can be
  // wrong, never a fiction. A phase-aware re-estimate (remaining projected segments) is a later
  // refinement; total-minus-elapsed is the honest first cut.
  float etaSeconds() const {
    const float remaining = total_ - static_cast<float>(elapsedMs_) / 1000.0f;
    return remaining > 0.0f ? remaining : 0.0f;
  }

  // --- phase model -----------------------------------------------------------------------------

  size_t phaseCount() const { return draft_.phaseCount; } // authored M for "N of M"
  size_t currentPhaseIndex() const { return curPhase_; }  // authored index, or kNoPhase/kCoolPhase
  bool inCooldown() const { return curPhase_ == kCoolPhase; }

  // 1-based ordinal for "Soak 2/4"; clamped into [1, phaseCount]. During the cool tail it reads the
  // last authored phase's ordinal (the UI shows "Cool" via inCooldown(), not a number past M).
  size_t phaseOrdinal() const {
    if (draft_.phaseCount == 0) {
      return 0;
    }
    if (curPhase_ < draft_.phaseCount) {
      return curPhase_ + 1;
    }
    return draft_.phaseCount; // cool tail / not-yet-started clamp
  }

  // Fraction [0,1] of the CURRENT authored phase's HOLD already delivered — what B6's resume
  // generator scales the remaining soak/UV dose by (§15).
  //
  // Read off the compiled timeline rather than wall-clock: a phase lowers to a ramp segment then a
  // hold segment, and the controller reports which segment it is in plus the run-into elapsed time,
  // so "how much soak happened" is (elapsed − start of the hold segment) / its duration. Doing it
  // from elapsed alone would count the ramp as soak, and on an ASAP ramp that ran long it would
  // report a phase as finished that never held at all.
  //
  // 0 while still ramping (no soak yet), 1 once past the phase. The hold is the LAST segment mapped
  // to the phase — B1 emits at most ramp+hold and omits a degenerate one, so a phase with no hold
  // segment at all reads 1 (nothing to redo).
  float holdProgress01() const {
    if (segCount_ == 0 || curPhase_ >= draft_.phaseCount) {
      return 1.0f; // not in an authored phase — nothing of one is outstanding
    }
    // Locate the phase's last segment (its hold) and that segment's start time.
    size_t holdSeg = segCount_;
    uint32_t startMs = 0;
    uint32_t accum = 0;
    for (size_t i = 0; i < segCount_ && i < kMaxSegments; ++i) {
      if (segPhase_[i] == curPhase_) {
        holdSeg = i;
        startMs = accum;
      }
      accum += segDurMs_[i];
    }
    if (holdSeg >= segCount_ || segDurMs_[holdSeg] == 0) {
      return 1.0f;
    }
    if (segIdx_ < holdSeg) {
      return 0.0f; // still ramping toward the target — none of the soak has happened
    }
    if (segIdx_ > holdSeg) {
      return 1.0f; // past this phase entirely
    }
    if (elapsedMs_ <= startMs) {
      return 0.0f;
    }
    const float frac =
        static_cast<float>(elapsedMs_ - startMs) / static_cast<float>(segDurMs_[holdSeg]);
    return frac > 1.0f ? 1.0f : frac;
  }

  // The current phase's operator-visible name ("Cool" in the tail, "" before the run enters one).
  const char *phaseName() const {
    if (curPhase_ == kCoolPhase) {
      return "Cool";
    }
    if (curPhase_ < draft_.phaseCount) {
      return draft_.phases[curPhase_].name;
    }
    return "";
  }

  // --- deviation / summary ---------------------------------------------------------------------

  // The §15 live amber cue — the SAME object §16's summary reads (run_fit.h), so the live warning
  // and the Done summary can never disagree.
  bool deviating() const { return fit_.liveDeviationCue(); }
  const RunFitAccumulator &fit() const { return fit_; }

  // The §16 fit verdict at Done (or a zeroed result for Stopped/Fault — data incomplete).
  RunFitResult finish(RunOutcome outcome, uint32_t nowMs) { return fit_.finish(nowMs, outcome); }

  // --- raw live readouts (for the header/readout the screen binds) -----------------------------

  uint32_t elapsedMs() const { return elapsedMs_; }
  uint32_t segIdx() const { return segIdx_; }
  oven_RunState runState() const { return static_cast<oven_RunState>(runState_); }
  RecipeMode mode() const { return mode_; }

private:
  // The authored phase a segment index maps to (kCoolPhase for the implicit cool tail). seg_idx
  // past the compiled count clamps to the last segment (a run at/after its final segment).
  size_t phaseForSeg(uint32_t seg) const {
    if (segCount_ == 0) {
      return kNoPhase;
    }
    const size_t s = seg < segCount_ ? static_cast<size_t>(seg) : segCount_ - 1;
    const uint8_t ph = segPhase_[s];
    return ph == kCoolSegment ? kCoolPhase : static_cast<size_t>(ph);
  }

  // The mode's control variable (§5): reflow tracks the workpiece TC; cure tracks the hottest wall
  // channel (a chamber-air proxy at 80 °C). Returned raw (may be NaN if a TC faulted — update()
  // finite-guards before it reaches the stats).
  float controlTemp(const oven_Telemetry &t) const {
    if (mode_ == RecipeMode::Cure) {
      float hottest = -1.0e30f;
      const size_t n = t.wall_temp_count <= 4 ? t.wall_temp_count : 4;
      for (size_t i = 0; i < n; ++i) {
        if (t.wall_temp[i] > hottest) {
          hottest = t.wall_temp[i];
        }
      }
      return n > 0 ? hottest : t.work_temp;
    }
    return t.work_temp;
  }

  ProfileDraft draft_{};
  const OvenModel *model_ = nullptr;
  RecipeMode mode_ = RecipeMode::Reflow;
  bool started_ = false;

  // Segment->phase map captured from the compile (parallel to the executed recipe.segments).
  uint8_t segPhase_[kMaxSegments] = {};
  uint32_t segDurMs_[kMaxSegments] = {};
  size_t segCount_ = 0;

  // Projected achievable curve + total, computed once at begin().
  profile_facts::CurvePoint proj_[profile_facts::kMaxCurvePoints] = {};
  size_t projN_ = 0;
  float total_ = 0.0f;

  // Live state from the latest telemetry tick.
  uint32_t segIdx_ = 0;
  uint32_t elapsedMs_ = 0;
  int runState_ = oven_RunState_RUN_STATE_IDLE;
  size_t curPhase_ = kNoPhase;

  RunFitAccumulator fit_;
};
