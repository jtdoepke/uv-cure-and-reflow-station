// TinyFrame configuration for the CYD<->controller UART link (design.md §9).
//
// This is the project-owned half of lib/tinyframe: the library source itself is
// the pristine MightyPork/TinyFrame git submodule in TinyFrame/, which expects
// the integrator to provide this header. BOTH peers compile this same file, so
// the frame format can never diverge between the boards.
//
// Derived from TinyFrame/TF_Config.example.h; deviations are commented.

#ifndef TF_CONFIG_H
#define TF_CONFIG_H

#include <stdint.h>
#include <stdio.h> // used by the TF_Error() macro defined below

//----------------------------- FRAME FORMAT ---------------------------------
// ,-----+-----+-----+------+------------+- - - -+-------------,
// | SOF | ID  | LEN | TYPE | HEAD_CKSUM | DATA  | DATA_CKSUM  |
// | 0-1 | 1-4 | 1-4 | 1-4  | 0-4        | ...   | 0-4         | <- size (bytes)
// '-----+-----+-----+------+------------+- - - -+-------------'

// !!! BOTH PEERS MUST USE THE SAME SETTINGS !!!

#define TF_ID_BYTES 1
#define TF_LEN_BYTES 2
#define TF_TYPE_BYTES 1 // message ids live in lib/protocol/messages.h

// CRC-16 over the frame body per the §9 link-layer table (upgradeable to CRC-32).
#define TF_CKSUM_TYPE TF_CKSUM_CRC16

// SOF byte on: the UART link is noisy-capable and the parser resyncs on it.
#define TF_USE_SOF_BYTE 1
#define TF_SOF_BYTE 0x01

//----------------------- PLATFORM COMPATIBILITY ----------------------------

typedef uint16_t TF_TICKS; // timeout tick counters
typedef uint8_t TF_COUNT;  // listener-loop counters

//----------------------------- PARAMETERS ----------------------------------

// Sized for the largest message. The historical default is a 32-segment Recipe (~1 KiB).
// The §2 "CYD is a UI remote" split (2026-07-17) added profile-management frames whose worst
// case is a full-library ProfileList (32 ProfileSummary rows) at 1542 B and a 32-phase
// ProfilePut/ProfileData at 1486 B (oven.pb.h *_size constants), which need 2048. That bump is
// applied PER-ENV via build_flags (-D TF_MAX_PAYLOAD_RX=2048 -D TF_SENDBUF_LEN=2048), not here,
// because it costs ~2 KiB of static DRAM and the production CYD has none to spare until the
// on-CYD profile/settings stores + WiFi leave it (Wave R3/R4). The controller (room to spare)
// and the host test/fuzz envs take the bump now; the CYD envs take it once the reclaim lands.
// The #ifndef lets an env override without editing this shared header.
#ifndef TF_MAX_PAYLOAD_RX
#define TF_MAX_PAYLOAD_RX 1024
#endif
#ifndef TF_SENDBUF_LEN
#define TF_SENDBUF_LEN 1024
#endif

#define TF_MAX_ID_LST 10
#define TF_MAX_TYPE_LST 10
#define TF_MAX_GEN_LST 5

// Ticks (TF_Tick() calls) before an incomplete frame is abandoned.
#define TF_PARSER_TIMEOUT_TICKS 10

// Single-threaded on both MCUs (one loop() owns the link) — no TX mutex.
#define TF_USE_MUTEX 0

// The production controller's link OWNS UART0 (design.md §2/§25) and Arduino-ESP32's printf
// lands there, so a parser error would put "[TF] ..." bytes on the wire — corrupting the very
// link it is complaining about, and doing it exactly when the link is already unhappy. The
// esp32dev_control env defines TF_ERROR_QUIET for that reason. Everyone else keeps the
// diagnostics: the host tests, and the bench build, whose console is a separate UART.
#if defined(TF_ERROR_QUIET)
#define TF_Error(format, ...) ((void)0)
#else
#define TF_Error(format, ...) printf("[TF] " format "\n", ##__VA_ARGS__)
#endif

//------------------------- End of user config ------------------------------

#endif // TF_CONFIG_H
