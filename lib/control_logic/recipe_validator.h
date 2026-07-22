// RecipeValidator — the controller's upload-time recipe/start checks (A7).
//
// Plugs into the ISetupValidator seam (lib/protocol/setup_responder.h): the
// SetupResponder calls validateRecipe/validateStart before Acking, so returning
// false with a NakReason turns the upload into a Nak{seq,reason} on the wire and
// applies no side effect (design.md §9). This replaces A2's accept-all default.
//
// The checks (design.md §4 "Mode-dependent temp cap" + §9 Nak reasons):
//   - structural: a recipe must have >=1 segment and every segment a non-zero
//     duration, else NAK_OUT_OF_RANGE.
//   - mode/content mismatch (NAK_MODE_CONTENT_MISMATCH): a recipe *tagged* REFLOW
//     that asserts uv/motor, or one mixing uv with a setpoint above the cure
//     ceiling — the untrusted tag disagreeing with the content is a hard reject.
//   - range (NAK_OUT_OF_RANGE): every setpoint within [floor, hard-max], where the
//     hard-max is chosen from *content* (oven_safety::deriveMode), so uv/motor
//     tighten the whole run to the cure ceiling regardless of the tag.
// validateStart requires the referenced recipe to be the one most recently
// accepted (NAK_UNKNOWN_RECIPE) — a stateless "the controller only starts what it
// just validated" check, plus the two guards below.
//
// The two Start guards A7 deferred, landed once their blockers did (A6 shipped both
// the run state and the temp-input port). Both exist because the CYD already refuses
// these in ConfirmRunScreen (§19) and §4/§9 require the controller to hold the same
// line without trusting it:
//   - NAK_ILLEGAL_TRANSITION: a Start while a run is already RUNNING. A genuine
//     retransmit never reaches here (SetupResponder dedups on seq), so a Start with a
//     fresh seq mid-run is a second run request. Read straight off the ProfileExecutor
//     rather than pushed in per-loop: the executor already IS the authority on run
//     state, and a cached flag could be a tick stale at exactly the moment this
//     decides. Optional, like the probe source below.
//   - NAK_WORKPIECE_TC_INVALID: a REFLOW-content Start whose workpiece probe is not
//     attached and reading like the load. Reflow is *controlled* on that channel, so
//     a dangling probe makes the whole run a lie. The predicate is shared with the
//     CYD's arm gate (oven_domain::workpieceTcPlausible, lib/calibration/
//     workpiece_tc.h) — a private copy here would drift into a Start the CYD offers
//     and this then refuses. Optional: with no IThermocouples wired the check is
//     skipped, which is what keeps the existing construction sites and the fuzz
//     harness unchanged.
// Note the layering: these are UPLOAD-time checks on the accept path. Runtime
// measured-temp trips are A4b's SafetySupervisor, and they cut outputs rather than
// Nak a frame.
//
// Header-only, no Arduino: unit-testable in native_control.
#pragma once

#include <cmath>
#include <cstdint>

#include "IThermocouples.h"
#include "codec.h"
#include "oven.pb.h"
#include "oven_safety.h"
#include "profile_executor.h"
#include "setup_responder.h"
#include "workpiece_tc.h"

class RecipeValidator : public protocol::ISetupValidator {
public:
  // Both sources are optional and borrowed, and each disables only its own guard when null — so
  // every existing construction site (the many bare `RecipeValidator v;` in tests, fuzz_pipeline.h,
  // a controller build with no temp adapter) keeps its previous behaviour. Both must outlive this.
  explicit RecipeValidator(const IThermocouples *tc = nullptr,
                           const ProfileExecutor *exec = nullptr)
      : tc_(tc), exec_(exec) {}

  bool validateRecipe(const oven_Recipe &recipe, oven_NakReason &reason) override {
    if (recipe.segments_count == 0) {
      reason = oven_NakReason_NAK_OUT_OF_RANGE;
      return false;
    }

    bool has_uv = false;
    bool has_motor = false;
    float min_heat = recipe.segments[0].heat_c;
    float max_heat = recipe.segments[0].heat_c;
    for (pb_size_t i = 0; i < recipe.segments_count; ++i) {
      const oven_Segment &seg = recipe.segments[i];
      // NaN/Inf off the wire would sail through every magnitude guard below (all
      // comparisons against NaN are false), so reject non-finite setpoints here —
      // this is the untrusted-CYD backstop, it cannot assume well-formed floats.
      if (seg.dur_ms == 0 || !std::isfinite(seg.heat_c)) {
        reason = oven_NakReason_NAK_OUT_OF_RANGE;
        return false;
      }
      has_uv = has_uv || seg.uv;
      has_motor = has_motor || seg.motor;
      min_heat = seg.heat_c < min_heat ? seg.heat_c : min_heat;
      max_heat = seg.heat_c > max_heat ? seg.heat_c : max_heat;
    }

    // Mode/content mismatch: the declared tag must not contradict the content. The tag is
    // untrusted, so read it via wireEnum — nanopb can leave recipe.mode holding a raw wire
    // value outside the enumerators, and an enum-typed load of that is UB (a fuzzer found it).
    if (protocol::wireEnum(recipe.mode) == oven_Mode_MODE_REFLOW && (has_uv || has_motor)) {
      reason = oven_NakReason_NAK_MODE_CONTENT_MISMATCH;
      return false;
    }
    if (has_uv && max_heat > oven_safety::CURE_HARD_MAX_C) {
      reason = oven_NakReason_NAK_MODE_CONTENT_MISMATCH;
      return false;
    }

    // Range: setpoints within [floor, content-derived hard-max].
    const float cap = oven_safety::hardMaxForMode(oven_safety::deriveMode(recipe));
    if (max_heat > cap || min_heat < oven_safety::MIN_SEGMENT_C) {
      reason = oven_NakReason_NAK_OUT_OF_RANGE;
      return false;
    }

    last_accepted_id_ = recipe.id;
    // Cache the CONTENT-derived mode alongside the id, so validateStart's probe check need not
    // re-derive it — and, more to the point, so it keys off the same verdict the cap above was
    // chosen from rather than off the untrusted `mode` tag.
    last_accepted_reflow_ = oven_safety::deriveMode(recipe) == oven_Mode_MODE_REFLOW;
    have_accepted_ = true;
    return true;
  }

  bool validateStart(const oven_Start &start, oven_NakReason &reason) override {
    // Session 0 is the IDLE sentinel in telemetry (src_control/main.cpp), so a run
    // must never adopt it: an accepted Start{session=0} would read as active on the
    // controller while telemetry still reported IDLE. The CYD already picks non-zero
    // (esp_random() | 1U); the backstop enforces it independently.
    if (start.session == 0) {
      reason = oven_NakReason_NAK_OUT_OF_RANGE;
      return false;
    }
    if (!have_accepted_ || start.recipe_id != last_accepted_id_) {
      reason = oven_NakReason_NAK_UNKNOWN_RECIPE;
      return false;
    }
    // A run is already executing. The CYD cannot reach this on any sanctioned path — §15's cure
    // resume, A11's orphan-end and §16's "Run again" all leave the executor IDLE or DONE before
    // re-Starting — so a Start arriving here is a stray, a duplicate that outlived the seq dedup,
    // or a CYD that has lost track of the machine. Refuse it rather than silently restarting a
    // live run under the operator (§9).
    if (exec_ != nullptr && exec_->state() == oven_RunState_RUN_STATE_RUNNING) {
      reason = oven_NakReason_NAK_ILLEGAL_TRANSITION;
      return false;
    }
    if (last_accepted_reflow_ && !workpieceProbeUsable()) {
      reason = oven_NakReason_NAK_WORKPIECE_TC_INVALID;
      return false;
    }
    return true;
  }

private:
  // Reflow's control sensor is the workpiece probe (§5), so a run started on a probe that is not
  // on the load tracks nothing. Skipped entirely when no IThermocouples is wired.
  bool workpieceProbeUsable() const {
    if (tc_ == nullptr) {
      return true;
    }
    const TcReading w = tc_->workpiece();
    const int n = tc_->wallCount();
    float hottest = oven_domain::kWallRefSeedC;
    for (int i = 0; i < n; ++i) {
      const TcReading r = tc_->wall(i);
      if (!r.fault) {
        hottest = oven_domain::foldWallRef(hottest, r.celsius);
      }
    }
    return oven_domain::workpieceTcPlausible(w.celsius, w.fault, /*haveWallRef=*/n > 0, hottest);
  }

  const IThermocouples *tc_ = nullptr;
  const ProfileExecutor *exec_ = nullptr;
  bool have_accepted_ = false;
  bool last_accepted_reflow_ = false;
  uint32_t last_accepted_id_ = 0;
};
