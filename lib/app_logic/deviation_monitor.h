// deviation_monitor.h — one residual channel: |actual − reference| against a band, with §16's
// sustained-time qualifier and running max/RMS statistics (design.md §15/§16, backlog B7).
//
// Streaming BY CONSTRUCTION — that is the whole design. §15's live deviation cue feeds it one
// telemetry sample per tick (4 Hz) and reads deviating(); §16's end-of-run summary reads stats()
// off the SAME object at Done. There is no separate batch path, which is what makes §16's "the
// same residual math as the live cue (§15), shared `lib/` logic" literally true rather than
// aspirational — and what makes "the readout was amber during the run" ⟺ "the summary flags it",
// by construction rather than by two implementations agreeing.
//
// The alternative — re-read the SD log at Done and recompute — would have the ESP32 re-parsing a
// multi-MB protobuf log to recover numbers it already had, with two code paths to keep in sync.
// Streaming keeps it O(1) memory.
//
// Time is a parameter, never a clock read (the thermal_math.h model-by-argument idiom), so D6's
// PC-side tools can replay a logged run through this same header offline and get identical
// numbers. That third consumer falls out for free.
//
// Lives in app_logic, not calibration: this is not math over the oven_cal.h constants (it never
// touches an OvenModel — the *projection* is B1/C7's job and this is downstream of it), the
// controller has no use for it, and its thresholds are hand-authored TBD §10 firmware constants
// rather than fit output that D6's emitter would regenerate.
#pragma once

#include <cmath>
#include <cstdint>

class DeviationMonitor {
public:
  struct Config {
    // §10 "Deviation/drift thresholds" (gates §8 step 4) defers all of these: "live-cue band
    // (§15) and end-of-run calibration-drift trigger (§16) — sustained-time N + RMSE/max
    // thresholds; firmware constants, tune against real runs". Nothing below has been measured
    // — they are order-of-magnitude PLACEHOLDERS, following the oven_cal.h CALIBRATED=false /
    // oven_safety.h CURE_HARD_MAX_C precedent. The §8-step-4 calibration campaign owns them.
    float bandC = 10.0F;            // TBD §10 — the §15 live-cue band
    uint32_t sustainedMs = 15000;   // TBD §10 — §16's "sustained > N s", so a transient door-open
                                    // spike doesn't trip it
    uint32_t maxSampleGapMs = 2000; // dt clamp: a logging stall must not weight one sample like a
                                    // minute of run (4 Hz nominal → 250 ms)
  };

  // POD — B8·1 writes this straight into the SD log header (§7: "both metrics are written to the
  // SD log header").
  struct Stats {
    uint32_t sampleCount;
    uint32_t spanMs;
    float lastResidualC;
    float maxAbsC;               // §16's "max ... deviation"
    float rmsC;                  // §16's "RMSE / RMS deviation"
    float meanC;                 // SIGNED — separates a bias (behind/overshooting) from noise
    uint32_t longestExcursionMs; // longest CONTINUOUS stretch beyond the band
    uint32_t totalOutOfBandMs;
    bool sustainedExceeded; // longestExcursionMs >= sustainedMs — the §16 trigger
  };

  explicit DeviationMonitor(Config cfg) : cfg_(cfg) {}
  DeviationMonitor() : DeviationMonitor(Config{}) {}

  // One sample. `referenceC` is the projected curve (run-quality channel) or the measured
  // workTemp (estimator channel). Weighted by dt so a 4 Hz live feed and a variable-rate log
  // replay agree; the first sample seeds the clock without weight.
  void addSample(uint32_t nowMs, float actualC, float referenceC) {
    const float residual = actualC - referenceC;
    const float absResidual = residual < 0.0F ? -residual : residual;
    lastResidualC_ = residual;
    ++sampleCount_;
    if (absResidual > maxAbsC_) {
      maxAbsC_ = absResidual;
    }

    if (!seeded_) { // first sample: no interval to attribute to it yet
      seeded_ = true;
      firstMs_ = nowMs;
      lastMs_ = nowMs;
      outOfBand_ = absResidual > cfg_.bandC;
      excursionMs_ = 0;
      return;
    }

    uint32_t dt = nowMs - lastMs_; // uint32 subtraction wraps correctly at the millis() rollover
    if (dt > cfg_.maxSampleGapMs) {
      dt = cfg_.maxSampleGapMs;
    }
    lastMs_ = nowMs;

    // The interval [prev, now] is attributed to this sample's residual (rectangular rule). Double
    // for the accumulators: a 10-minute reflow at 4 Hz is ~2400 samples of r²·dt and float32
    // visibly loses the tail.
    const double dtd = static_cast<double>(dt);
    sumDt_ += dtd;
    sumResidualDt_ += static_cast<double>(residual) * dtd;
    sumSqDt_ += static_cast<double>(residual) * static_cast<double>(residual) * dtd;

    const bool nowOut = absResidual > cfg_.bandC;
    if (nowOut) {
      totalOutOfBandMs_ += dt;
      excursionMs_ = outOfBand_ ? excursionMs_ + dt : dt; // continue vs. start an excursion
      if (excursionMs_ > longestExcursionMs_) {
        longestExcursionMs_ = excursionMs_;
      }
    } else {
      excursionMs_ = 0; // back in band — the next excursion starts fresh
    }
    outOfBand_ = nowOut;
  }

  // Instantaneous: |last residual| beyond the band.
  bool outOfBand() const { return outOfBand_; }

  // Sustained: the CURRENT excursion has held for >= sustainedMs. This is what §15's amber
  // readout binds to — same window as §16's flag, so the live cue and the summary are the same
  // statement rather than two thresholds that merely resemble each other.
  bool deviating() const { return outOfBand_ && excursionMs_ >= cfg_.sustainedMs; }

  float lastResidualC() const { return lastResidualC_; } // signed

  Stats stats() const {
    Stats s{};
    s.sampleCount = sampleCount_;
    s.spanMs = seeded_ ? lastMs_ - firstMs_ : 0;
    s.lastResidualC = lastResidualC_;
    s.maxAbsC = maxAbsC_;
    s.longestExcursionMs = longestExcursionMs_;
    s.totalOutOfBandMs = totalOutOfBandMs_;
    s.sustainedExceeded = longestExcursionMs_ >= cfg_.sustainedMs;
    if (sumDt_ > 0.0) {
      s.meanC = static_cast<float>(sumResidualDt_ / sumDt_);
      s.rmsC = static_cast<float>(std::sqrt(sumSqDt_ / sumDt_));
    }
    return s;
  }

  void reset() { *this = DeviationMonitor(cfg_); }

private:
  Config cfg_;
  bool seeded_ = false;
  bool outOfBand_ = false;
  uint32_t sampleCount_ = 0;
  uint32_t firstMs_ = 0;
  uint32_t lastMs_ = 0;
  float lastResidualC_ = 0.0F;
  float maxAbsC_ = 0.0F;
  double sumDt_ = 0.0;
  double sumResidualDt_ = 0.0;
  double sumSqDt_ = 0.0;
  uint32_t excursionMs_ = 0;
  uint32_t longestExcursionMs_ = 0;
  uint32_t totalOutOfBandMs_ = 0;
};
