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

// Setup path (Recipe/Start): resend interval and retry budget before the sender
// gives up and surfaces an error. kSetupMaxRetries counts resends *after* the
// initial send, so a command is transmitted up to 1 + kSetupMaxRetries times.
inline constexpr uint32_t kSetupAckTimeoutMs = 200;
inline constexpr uint8_t kSetupMaxRetries = 3;

// Boot handshake: resend Hello this often until a peer Hello is seen (covers the
// race where one board powers up first).
inline constexpr uint32_t kHelloRetryMs = 200;

} // namespace protocol
