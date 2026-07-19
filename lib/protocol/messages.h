// Message-level view of the shared wire contract: nanopb-generated structs plus
// the TinyFrame type id assigned to each message (design.md §9).
#pragma once

#include <cstdint>

#include "oven.pb.h"

namespace protocol {

// TinyFrame frame-type ids.
//
// kTfTypeHello is part of the FROZEN append-only bootstrap contract along with
// the Hello message itself (see proto/oven.proto): it must never change, so any
// two firmware versions can always exchange Hello and compare schema hashes.
inline constexpr uint8_t kTfTypeHello = 0x01;

// Everything below is gated by the schema-hash handshake and may churn freely
// (renumber, add, remove) as long as both boards are flashed together.
inline constexpr uint8_t kTfTypeRecipe = 0x10;
inline constexpr uint8_t kTfTypeStart = 0x11;
inline constexpr uint8_t kTfTypeHeartbeat = 0x12;
inline constexpr uint8_t kTfTypeAbort = 0x13;
inline constexpr uint8_t kTfTypeAck = 0x14;
inline constexpr uint8_t kTfTypeNak = 0x15;
inline constexpr uint8_t kTfTypeTelemetry = 0x16;
inline constexpr uint8_t kTfTypeDone = 0x17;
inline constexpr uint8_t kTfTypeFault = 0x18;

// Profile & settings management (design.md §9; added 2026-07-17 with the §2 "CYD is a UI
// remote" split). Request/reply path — also gated by the schema handshake, so these ids may
// churn as long as both boards are flashed together. Requests carry a seq; the matching reply
// echoes it.
inline constexpr uint8_t kTfTypeProfileListReq = 0x19;
inline constexpr uint8_t kTfTypeProfileGetReq = 0x1A;
inline constexpr uint8_t kTfTypeProfilePut = 0x1B;
inline constexpr uint8_t kTfTypeProfileDelete = 0x1C;
inline constexpr uint8_t kTfTypeProfileDup = 0x1D;
inline constexpr uint8_t kTfTypeProfileRename = 0x1E;
inline constexpr uint8_t kTfTypeSettingsGetReq = 0x1F;
inline constexpr uint8_t kTfTypeSettingsPut = 0x20;
inline constexpr uint8_t kTfTypeProfileList = 0x21;
inline constexpr uint8_t kTfTypeProfileData = 0x22;
inline constexpr uint8_t kTfTypeSettingsData = 0x23;
inline constexpr uint8_t kTfTypeMgmtResult = 0x24;
inline constexpr uint8_t kTfTypeProfileTouch = 0x25;

} // namespace protocol
