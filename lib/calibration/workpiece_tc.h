// workpiece_tc.h — the ONE definition of "is the workpiece probe attached and reading like the
// load" for the whole codebase (design.md §6, §19; backlog A7).
//
// Two subsystems on BOTH MCUs have to agree on this predicate, and they reach the same readings
// through different pipes:
//   - the CYD's §19 Confirm screen gates the press-and-hold arm on it, reading `work_temp` +
//     `wall_temp[]` out of a Telemetry frame (ui_logic/confirm_run_screen.cpp);
//   - the controller NAKs a reflow Start on it (NAK_WORKPIECE_TC_INVALID, §9), reading the
//     IThermocouples port directly (control_logic/recipe_validator.h).
// If those two copies drift the CYD offers an arm the controller then refuses — a dead button with
// no explanation. This header is the single source; each site derives its rule from it. Exactly the
// situation touch_safe.h was written for, and the same remedy.
//
// Why a shared compile-time predicate does NOT weaken the §4/§9 "never trust the CYD" rule: that
// rule is about runtime data flow — the controller must not accept a limit or a verdict pushed over
// the wire. This is reviewed code baked into each firmware at build time, and the controller still
// evaluates it against its OWN sensor readings, never the CYD's. Sharing the source only guarantees
// both firmwares were built from the same reviewed rule.
//
// What it actually tests, and why (§6): a workpiece thermocouple that is unplugged, open, or
// shorted reads as NaN or as a far-out-of-band sentinel — so a plain physical-range band catches
// most of it. The band alone cannot catch a probe dangling in the chamber instead of clipped to the
// board, though, and that one matters most: reflow is controlled ON this channel, so a probe that
// is not on the load makes the whole run a lie. The cross-check is the tell — a real workpiece can
// read *cooler* than the chamber walls (a cold board in a warm oven) but never implausibly hotter.
//
// Lives in calibration/ for the same reason touch_safe.h does: it is the codebase's board-neutral,
// both-firmware thermal-domain home. It is a reviewed constant set, NOT a calibration output, and
// must never migrate into the generated oven_cal.h.
//
// Pure C++: no LVGL, no Arduino, no protobuf, no control_port — plain floats, so either side can
// feed it from whatever shape its readings arrive in.
#pragma once

namespace oven_domain {

// Physical band for a real thermocouple on a real workpiece. Below the floor is an open/short
// sentinel rather than a temperature; above the ceiling is outside anything this oven can do.
inline constexpr float kWorkpieceMinPlausibleC = -10.0f;
inline constexpr float kWorkpieceMaxPlausibleC = 300.0f;

// How far above the hottest chamber wall a genuine workpiece reading may sit. Slack, not zero: the
// probe is a small mass in a moving air stream and can briefly lead the wall it is measured
// against. Well clear of that, and still far below where a dangling probe would land.
inline constexpr float kWorkpieceWallSlackC = 30.0f;

// Seed for the wall-reference fold below. Any real reading beats it, so a fold that returns the
// seed unchanged means "every wall channel was unusable".
inline constexpr float kWallRefSeedC = -1.0e30f;

// Fold one wall channel into the running cross-check reference (hottest wins). A NaN channel
// compares false and is therefore skipped, which is the intent — an unreadable wall contributes no
// evidence. Seed the accumulator with kWallRefSeedC.
//
// Shared rather than open-coded at both call sites so the skip rule and the seed cannot drift
// apart from the predicate that consumes them.
inline float foldWallRef(float acc, float wallC) {
  return wallC > acc ? wallC : acc;
}

// Is this workpiece reading consistent with a probe attached to the load?
//
//   workC       the workpiece probe's reading, deg C.
//   workFault   the front-end's own open/short flag, where the caller has one. Pass false where
//               none exists (a Telemetry frame carries no per-channel flag) — a faulted probe still
//               surfaces as NaN or an out-of-band value, which the checks below catch.
//   haveWallRef whether the caller had any wall channels at all to fold. False skips the
//               cross-check entirely: with no chamber reference there is nothing to compare
//               against, and rejecting on that basis would refuse every run on a wall-less rig.
//   hottestWallC the folded reference (see foldWallRef). With haveWallRef true but every channel
//               unusable this is still kWallRefSeedC, and the cross-check then fails — deliberately
//               fail-closed, since an oven that cannot read its own walls offers no evidence the
//               probe is where it claims to be.
inline bool workpieceTcPlausible(float workC, bool workFault, bool haveWallRef,
                                 float hottestWallC) {
  if (workFault) {
    return false;
  }
  if (!(workC == workC)) {
    return false; // NaN -> open/faulted probe
  }
  if (workC < kWorkpieceMinPlausibleC || workC > kWorkpieceMaxPlausibleC) {
    return false; // open-circuit sentinel / outside the oven's physical range
  }
  // Running implausibly hotter than the chamber: dangling in free air or miswired, not on the load.
  if (haveWallRef && workC > hottestWallC + kWorkpieceWallSlackC) {
    return false;
  }
  return true;
}

} // namespace oven_domain
