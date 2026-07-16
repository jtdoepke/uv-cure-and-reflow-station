// Link cadence constants — the accepted defaults from the design.md §9 table.
// Shared by both sides so the CYD's send cadence and the controller's timeout
// budget stay defined in one place.
#pragma once

#include <cstdint>

namespace protocol {

// CYD -> controller heartbeat period.
inline constexpr uint32_t kHeartbeatPeriodMs = 200;

// Controller safes if no valid heartbeat for the active session arrives within
// this window (~3-4 missed heartbeats).
inline constexpr uint32_t kCommandTimeoutMs = 750;

// Controller -> CYD telemetry period. Sent unconditionally, run or no run: it is
// the CYD's only evidence the controller is alive at all, and §9's re-sync has the
// controller emit IDLE telemetry from boot.
inline constexpr uint32_t kTelemetryPeriodMs = 250;

// The CYD reads the link as down after this long with no telemetry (~4 missed
// frames — the same "miss 3-4 and act" logic as kCommandTimeoutMs, in the other
// direction). Deliberately NOT the same number as FaultController::linkTimeoutMs:
// this drives Home's indicator + run-flow gate (§14), which should be honest within
// a second, whereas that one decides when to throw a red mid-run modal (§22) and is
// deliberately more patient. **TBD §10** — both are unmeasured placeholders.
inline constexpr uint32_t kLinkTimeoutMs = 1000;

// How often each firmware's loop must call FrameLink::tick(). TinyFrame counts its
// parser-resync timeout in TF_Tick() *calls*, not milliseconds (TF_PARSER_TIMEOUT_TICKS
// = 10, TF_Config.h), so this cadence is what converts that count into a real ~100 ms:
// a half-received frame is abandoned well inside kCommandTimeoutMs, and a truncated
// frame can never wedge the parser long enough to look like a dead link.
//
// Lives here rather than in each board header because it is a property of the protocol,
// not of a board: TF_PARSER_TIMEOUT_TICKS is one number shared by both firmwares, so the
// cadence that gives it meaning must be too. Both sides tick at the same rate for the
// same reason — that is one fact, and it gets one definition.
inline constexpr uint32_t kLinkTickMs = 10;

// Setup path (Recipe/Start): resend interval and retry budget before the sender
// gives up and surfaces an error. kSetupMaxRetries counts resends *after* the
// initial send, so a command is transmitted up to 1 + kSetupMaxRetries times.
inline constexpr uint32_t kSetupAckTimeoutMs = 200;
inline constexpr uint8_t kSetupMaxRetries = 3;

// Boot handshake: resend Hello this often until a peer Hello is seen (covers the
// race where one board powers up first).
inline constexpr uint32_t kHelloRetryMs = 200;

} // namespace protocol
