// run_fit.h — §16's run-summary fit verdict + calibration-drift advisory (backlog B7).
//
// Two DeviationMonitor channels fed from one telemetry tick, plus §16's per-phase target checks,
// resolved at Done into {verdict, cause, advisory}:
//
//   1. Run quality      — measured workTemp vs the projected curve. "Did the run follow the plan?"
//   2. Estimator quality — boardEst vs workTemp. "Is the planning model still telling the truth?"
//      §16: "every reflow run carries the measured workpiece TC, so the planning estimator is
//      checked against ground truth on every run — no longer a self-referential comparison
//      against a projection derived from the same calibration."
//
// The two channels are what make §16 "honest by design — and now discriminating": they separate
// the two causes the advisory's wording names, so it picks its wording instead of guessing.
//
// The §15 live cue reads liveDeviationCue() off the same object during the run — see
// deviation_monitor.h for why there is no separate batch path.
//
// This produces an INLINE AMBER ADVISORY, never the red fault overlay (§22: soft warnings
// "use inline amber banners, never this modal ... Keeping the red fault overlay rare is what
// preserves its force"). Nothing here feeds FaultController.
#pragma once

#include <cstdint>

#include "deviation_monitor.h"

// §16's outcome badge. Only Completed computes a fit — "abort/fault skip it — data incomplete".
//
// DoorOpened is its own outcome rather than a flavour of Stopped: §15 gives it different wording
// ("Run aborted — door opened"), §22 excludes it from the red modal, and — the reason it cannot be
// folded into Fault — the terminal page is DISMISSABLE by the door itself, which a fault must never
// be. It needs no special case in the fit rule: every non-Completed outcome already skips it.
enum class RunOutcome : uint8_t { Completed, Stopped, Fault, DoorOpened };

enum class FitVerdict : uint8_t { Good, Fair, Poor };

// §16's discrimination rule, verbatim: "A large `boardEst`-vs-`workTemp` residual points at the
// projection model (board-mass mismatch or estimator drift); a measured-vs-projected miss with a
// *clean* estimator residual points at the oven itself (element aging, door seal)."
enum class DriftCause : uint8_t {
  None,            // both residuals clean
  Oven,            // element aging, door seal
  ProjectionModel, // board-mass mismatch or estimator drift
};

// §16's "per-phase target checks — did soak/peak actually reach target and hold time-above-
// liquidus?"
struct PhaseTargetCheck {
  float targetC;
  float peakActualC;
  uint32_t timeAtOrAboveMs; // hold / time-above-liquidus, summed across dips
  uint32_t requiredHoldMs;
  bool reachedTarget;
  bool heldLongEnough;
};

// POD, fixed extent, no pointers — B8·1 memcpy's this into the SD log header (§7).
struct RunFitResult {
  bool computed; // false unless outcome == Completed (§16)
  DeviationMonitor::Stats runQuality;
  DeviationMonitor::Stats estimatorQuality;
  uint8_t phaseCount;
  uint8_t phasesMissedTarget;
  uint8_t phasesShortHold;
  FitVerdict verdict;
  DriftCause cause;
  bool advisory; // show the §16 amber advisory + the Calibration-workflow shortcut
};

class RunFitAccumulator {
public:
  // Matches `Recipe.segments max_count:32` in proto/oven.options — raise both together.
  static constexpr uint8_t MAX_PHASES = 32;

  struct Config {
    DeviationMonitor::Config runQuality{};
    DeviationMonitor::Config estimator{};
    // All TBD §10 ("Deviation/drift thresholds", gates §8 step 4). PLACEHOLDERS — unmeasured.
    float targetToleranceC = 3.0F; // TBD §10 — how close counts as "reached target"
    float fairMaxC = 8.0F;         // TBD §10 — verdict bands on the run-quality channel
    float poorMaxC = 20.0F;        // TBD §10
    float fairRmsC = 4.0F;         // TBD §10
    float poorRmsC = 10.0F;        // TBD §10
  };

  explicit RunFitAccumulator(Config cfg)
      : cfg_(cfg), runQuality_(cfg.runQuality), estimator_(cfg.estimator) {}
  RunFitAccumulator() : RunFitAccumulator(Config{}) {}

  void beginRun(uint32_t nowMs) {
    runQuality_ = DeviationMonitor(cfg_.runQuality);
    estimator_ = DeviationMonitor(cfg_.estimator);
    phaseCount_ = 0;
    phaseOpen_ = false;
  }

  // Closes any open phase first, so callers can just call beginPhase per segment.
  void beginPhase(uint32_t nowMs, float targetC, uint32_t requiredHoldMs) {
    endPhase(nowMs);
    if (phaseCount_ >= MAX_PHASES) {
      return; // saturate rather than overrun; a >32-segment recipe can't exist (oven.options)
    }
    PhaseTargetCheck &p = phases_[phaseCount_++];
    p = PhaseTargetCheck{};
    p.targetC = targetC;
    p.requiredHoldMs = requiredHoldMs;
    p.peakActualC = -1000.0F; // sentinel: below any real reading
    phaseOpen_ = true;
    phaseLastMs_ = nowMs;
    phaseAbove_ = false;
  }

  // ONE telemetry tick (§15, 4 Hz) feeds both channels and the open phase.
  //   workTempC  — Telemetry.work_temp (the cure variant passes wall temp; §15's cure curve
  //                plots wall temp, its control variable per §5)
  //   projectedC — C7's projected curve at this instant
  //   boardEstC  — Telemetry.board_est, the controller's {a,b,τ} estimate (§6)
  void addSample(uint32_t nowMs, float workTempC, float projectedC, float boardEstC) {
    runQuality_.addSample(nowMs, workTempC, projectedC);
    estimator_.addSample(nowMs, boardEstC, workTempC); // ground truth is the reference

    if (!phaseOpen_ || phaseCount_ == 0) {
      return;
    }
    PhaseTargetCheck &p = phases_[phaseCount_ - 1];
    if (workTempC > p.peakActualC) {
      p.peakActualC = workTempC;
    }
    // Time-above-liquidus accumulates across dips: a momentary drop below target doesn't reset
    // the hold, it just doesn't count toward it.
    const bool above = workTempC >= p.targetC - cfg_.targetToleranceC;
    if (above && phaseAbove_) {
      p.timeAtOrAboveMs += nowMs - phaseLastMs_;
    }
    phaseAbove_ = above;
    phaseLastMs_ = nowMs;
  }

  void endPhase(uint32_t nowMs) {
    if (!phaseOpen_ || phaseCount_ == 0) {
      return;
    }
    PhaseTargetCheck &p = phases_[phaseCount_ - 1];
    if (phaseAbove_) {
      p.timeAtOrAboveMs += nowMs - phaseLastMs_;
    }
    p.reachedTarget = p.peakActualC >= p.targetC - cfg_.targetToleranceC;
    p.heldLongEnough = p.timeAtOrAboveMs >= p.requiredHoldMs;
    phaseOpen_ = false;
  }

  // §16: completed runs only — "abort/fault skip it — data incomplete".
  RunFitResult finish(uint32_t nowMs, RunOutcome outcome) {
    endPhase(nowMs);
    RunFitResult r{};
    if (outcome != RunOutcome::Completed) {
      return r; // computed == false; every field zeroed
    }
    r.computed = true;
    r.runQuality = runQuality_.stats();
    r.estimatorQuality = estimator_.stats();
    r.phaseCount = phaseCount_;
    for (uint8_t i = 0; i < phaseCount_; ++i) {
      if (!phases_[i].reachedTarget) {
        ++r.phasesMissedTarget;
      } else if (!phases_[i].heldLongEnough) {
        ++r.phasesShortHold;
      }
    }

    const bool estimatorDirty = r.estimatorQuality.sustainedExceeded;
    const bool runDirty =
        r.runQuality.sustainedExceeded || r.phasesMissedTarget > 0 || r.phasesShortHold > 0;

    // A dirty estimator wins the attribution even when the run tracked fine: §16 checks the
    // estimator against ground truth "on every run", and a drifting estimator silently wrecks the
    // NEXT run's ETA and preview (§15) even though this one looked clean.
    if (estimatorDirty) {
      r.cause = DriftCause::ProjectionModel;
    } else if (runDirty) {
      r.cause = DriftCause::Oven;
    } else {
      r.cause = DriftCause::None;
    }
    r.advisory = estimatorDirty || runDirty;

    // §16 notes the run-quality check "should normally pass by construction" given §5's
    // hold-entry gate — Good being the common case is expected, not a bug.
    if (r.runQuality.sustainedExceeded || r.phasesMissedTarget > 0 ||
        r.runQuality.rmsC > cfg_.poorRmsC || r.runQuality.maxAbsC > cfg_.poorMaxC) {
      r.verdict = FitVerdict::Poor;
    } else if (r.phasesShortHold > 0 || r.runQuality.rmsC > cfg_.fairRmsC ||
               r.runQuality.maxAbsC > cfg_.fairMaxC) {
      r.verdict = FitVerdict::Fair;
    } else {
      r.verdict = FitVerdict::Good;
    }
    return r;
  }

  // The §15 live cue, same math, same object (§16's "same residual math as the live cue").
  bool liveDeviationCue() const { return runQuality_.deviating(); }

  const DeviationMonitor &runQualityChannel() const { return runQuality_; }
  const DeviationMonitor &estimatorChannel() const { return estimator_; }
  uint8_t phaseCount() const { return phaseCount_; }
  const PhaseTargetCheck &phase(uint8_t i) const { return phases_[i]; }

private:
  Config cfg_;
  DeviationMonitor runQuality_;
  DeviationMonitor estimator_;
  PhaseTargetCheck phases_[MAX_PHASES]{};
  uint8_t phaseCount_ = 0;
  bool phaseOpen_ = false;
  bool phaseAbove_ = false;
  uint32_t phaseLastMs_ = 0;
};

// §16's advisory copy. Lives here, not in the view — the fault_table.h precedent: operator-facing
// copy in app_logic is string-compared in tests instead of reviewed by eye.
//
// §16 quotes ONE paragraph covering both causes ("may mean the oven needs recalibration — or that
// this board's thermal mass differs from the calibration board"), then says the design now
// discriminates and "the advisory picks its wording accordingly" — without giving the two split
// texts. B7 drafted the split and flagged it for a human pass; **reviewed and rewritten
// 2026-07-21**, on four rules worth keeping if these are ever edited again:
//   - Conclusion first, evidence second. The drafts opened on `boardEst` vs `workTemp` — the
//     discriminator's internals, which answer a question the operator did not ask.
//   - Name the action. §16 pairs this advisory with a shortcut into the Calibration workflow, so
//     the text should point at recalibrating rather than merely diagnose.
//   - Say "PCB" for the workpiece. "Board" means the ESP32s everywhere else in this codebase, and
//     the ProjectionModel draft used it twice for the load.
//   - Keep the hedges ("points at", "most often", "may"). §16 is titled *honest by design*, and
//     these thresholds are still unmeasured §10 placeholders.
// GLYPH CONTRACT (learned the hard way — these strings rendered as missing-glyph boxes the first
// time C8 drew them): the fonts carry ASCII + `°` + `·` and a handful of Font Awesome symbols,
// nothing else (lib/ui_logic/fonts/README.md). So no em-dash, and NO leading warning sign: the
// literal `⚠` is U+26A0, while the only warning glyph that exists here is Font Awesome's 0xF071
// behind LV_SYMBOL_WARNING — a different codepoint, and an `lv_` name this LVGL-free header
// cannot reference anyway. The glyph is therefore the VIEW's to prepend, which is also what
// theme.h's alert helpers already require ("the redundant cue ... is the caller's job").
inline const char *advisoryText(DriftCause cause) {
  switch (cause) {
  case DriftCause::Oven:
    return "This run missed its predicted curve while the temperature model still matched the "
           "workpiece probe. That points at the oven, not the profile: usually an aging element "
           "or a worn door seal. Recalibrate to realign predictions.";
  case DriftCause::ProjectionModel:
    return "Predicted temperature drifted from what the workpiece probe measured, so the "
           "planning model no longer matches this load. Usually this PCB's thermal mass differs "
           "from the calibration board. Times may run optimistic until you recalibrate.";
  case DriftCause::None:
  default:
    return "";
  }
}
