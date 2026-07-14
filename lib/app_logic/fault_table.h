// fault_table.h — the CYD's `faultCode` → operator-wording map (design.md §22, backlog B7).
//
// The controller owns the `FaultCode` enum in the shared `.proto` (§9); this is the CYD half:
// each code → a plain-language title, the "what the system already did" reassurance line, the
// small `Code: OVERTEMP_CHAMBER` string from the §22 layout, and a severity used for one rule
// only — §22's "higher-priority fault while shown: update the overlay to the new cause and keep
// a `+N` count". Pure data + lookup: no `lv_`, no Arduino, host-tested in native_logic_cyd. The
// view (C8) only renders what this returns; keeping the operator-facing copy here is what makes
// the exact §22 wording a unit-test assertion instead of a UI review comment.
//
// The table keys off the generated `oven_FaultCode` rather than a CYD-side copy of the enum: a
// parallel enum here would be exactly the drift §9's matched-pair invariant exists to prevent.
// (This is why native_logic_cyd carries nanopb — see platformio.ini.)
//
// Nothing here decides a safety action. By the time a code reaches the CYD the controller has
// already safed itself (§4/§9); severity orders *presentation*, never behaviour.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "oven.pb.h"

namespace fault_table {

// Priority bands for §22's "higher-priority fault while shown" rule. Ordinal comparison IS the
// rule: a strictly higher severity replaces the displayed cause; equal or lower only bumps the
// count. Values are spaced so a new code can slot into a band without renumbering.
//
// The ordering is derived from §22's own "typical cause" column, reading it as "how much does
// this code tell us is physically wrong right now":
//   - a broken safety layer outranks a hazard, because it says the primary cut failed;
//   - a hazard outranks lost ground truth, because we can still see it;
//   - lost ground truth outranks a controller fault, because the controller safes by construction;
//   - anything that *measured* something outranks LINK_LOST, whose §22 wording deliberately
//     claims nothing about live state.
// design.md never defines this order (`severity` appears once, in §22's architecture note), so
// it is a B7 decision — see the backlog note. It is presentation-only, so it is cheap to revise.
enum class Severity : uint8_t {
  RunIntegrity = 10,      // the run failed; nothing is unsafe
  LinkIntegrity = 20,     // we cannot confirm live state at all
  ControllerFault = 30,   // the controller itself; outputs default OFF (§4 L2)
  SensorIntegrity = 40,   // the ground truth the limits depend on is gone
  ThermalHazard = 50,     // something is actually too hot
  UncommandedEnergy = 60, // the primary cut failed; heat applied without command
};

struct FaultInfo {
  const char *title;    // plain-language cause, largest text (§22 layout). nullptr iff !known.
  const char *guidance; // the "what the system already did (reassure)" line. nullptr iff !known.
  const char *codeName; // the small `Code: OVERTEMP_CHAMBER` line. nullptr iff !known.
  Severity severity;
  bool overTemp; // → HOT (§14) persists on Home and keeps suppressing sleep (§17) after ack.
  bool known;    // false → §22's generic wording via formatTitle(). Never blank.
};

// The reassurance line shared by every fault the controller safed itself on (§22 layout:
// "Heater & UV are OFF · run aborted"). LINK_LOST deliberately does NOT use this — see below.
inline constexpr const char *kSafedGuidance = "Heater & UV are OFF \xC2\xB7 run aborted";

// Total: defined for every value, including codes this build has never heard of.
//
// FAULT_NONE is deliberately !known — it is the enum's zero value, not a fault. A `Fault` frame
// carrying it is malformed, and FaultController drops it rather than latching a blank overlay.
inline FaultInfo faultInfo(oven_FaultCode code) {
  switch (code) {
  case oven_FaultCode_FAULT_OVERTEMP_CHAMBER:
    return {"Chamber over-temperature", kSafedGuidance,
            "OVERTEMP_CHAMBER",         Severity::ThermalHazard,
            /*overTemp=*/true,          true};
  case oven_FaultCode_FAULT_OVERTEMP_CASE:
    // Not overTemp: §14's HOT band and §17's sleep suppression are about a touchable *chamber*.
    // The electronics-case sensor (§6) says nothing about that, so flagging it would be a false
    // HOT that silently keeps the display awake.
    return {"Electronics over-temperature", kSafedGuidance, "OVERTEMP_CASE",
            Severity::ThermalHazard,        false,          true};
  case oven_FaultCode_FAULT_SENSOR_FAULT:
    return {"Temperature-sensor fault", kSafedGuidance, "SENSOR_FAULT",
            Severity::SensorIntegrity,  false,          true};
  case oven_FaultCode_FAULT_TC_IMPLAUSIBLE:
    return {"Work sensor not responding", kSafedGuidance, "TC_IMPLAUSIBLE",
            Severity::SensorIntegrity,    false,          true};
  case oven_FaultCode_FAULT_TARGET_UNREACHABLE:
    return {"Target not reachable", kSafedGuidance, "TARGET_UNREACHABLE",
            Severity::RunIntegrity, false,          true};
  case oven_FaultCode_FAULT_HEATER_STUCK:
    // §22: "temp rising at ~0 % duty — welded SSR; contactor opened". The only code that says a
    // safety layer itself failed, hence the top of the order.
    return {"Heater energized unexpectedly",
            "Mains contactor opened \xC2\xB7 run aborted",
            "HEATER_STUCK",
            Severity::UncommandedEnergy,
            /*overTemp=*/true,
            true};
  case oven_FaultCode_FAULT_RUNTIME_EXCEEDED:
    return {"Run exceeded time bound", kSafedGuidance, "RUNTIME_EXCEEDED",
            Severity::RunIntegrity,    false,          true};
  case oven_FaultCode_FAULT_LINK_LOST:
    // §22 verbatim. Both clauses are load-bearing and must not be trimmed: the timeout clause
    // covers a hung link with a live controller; the reset-default clause covers a controller
    // that isn't executing at all (e.g. held in its bootloader), where "it safes itself" would
    // be a claim about code that isn't running. Reassurance via the §4/§9 invariant — never a
    // live readback, because a silent link is exactly the case where we cannot confirm state.
    return {"Lost communication",
            "Lost communication with the controller. If a run was active, the heater is off "
            "\xE2\x80\x94 "
            "the controller safes on heartbeat timeout, and its outputs default OFF on any reset.",
            "LINK_LOST",
            Severity::LinkIntegrity,
            false,
            true};
  case oven_FaultCode_FAULT_WATCHDOG:
    return {"Controller reset (watchdog)",
            "Outputs defaulted OFF on reset \xC2\xB7 run aborted",
            "WATCHDOG",
            Severity::ControllerFault,
            false,
            true};
  case oven_FaultCode_FAULT_INTERNAL:
    return {"Controller fault", kSafedGuidance, "INTERNAL", Severity::ControllerFault, false, true};
  case oven_FaultCode_FAULT_NONE:
  default:
    // Unknown codes rank with INTERNAL, the existing catch-all: "a code I don't understand" and
    // "the controller says something went wrong internally" warrant identical treatment. Ranking
    // unknowns lowest would bury a genuinely new hazard code behind RUNTIME_EXCEEDED; ranking
    // them highest would let a corrupt frame preempt a real OVERTEMP_CHAMBER.
    return {nullptr, nullptr, nullptr, Severity::ControllerFault, false, false};
  }
}

// Renders the §22 display title for ANY code into `out`: the table title when known, else the
// generic "Fault <code> — oven safed to a safe state" — informative rather than blank, which §22
// requires as defence-in-depth even though the §9 matched-pair invariant should prevent it.
// Caller owns the buffer (no heap, no lv_); 64 bytes is ample. Always NUL-terminates.
inline void formatTitle(oven_FaultCode code, char *out, size_t n) {
  if (out == nullptr || n == 0) {
    return;
  }
  const FaultInfo info = faultInfo(code);
  if (info.known) {
    snprintf(out, n, "%s", info.title);
    return;
  }
  snprintf(out, n, "Fault %d \xE2\x80\x94 oven safed to a safe state", static_cast<int>(code));
}

// Strict: an equal-severity fault does NOT outrank the incumbent, so the first cause of a given
// severity keeps the overlay (§22 shows one cause + a count, so ties need a deterministic winner).
inline bool outranks(oven_FaultCode candidate, oven_FaultCode incumbent) {
  return static_cast<uint8_t>(faultInfo(candidate).severity) >
         static_cast<uint8_t>(faultInfo(incumbent).severity);
}

} // namespace fault_table
