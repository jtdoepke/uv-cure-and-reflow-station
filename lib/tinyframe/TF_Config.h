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

// Sized for the largest message: a Recipe with 32 segments (oven.options) is
// well under 1 KiB encoded. Revisit with real numbers in backlog A1.
#define TF_MAX_PAYLOAD_RX 1024
#define TF_SENDBUF_LEN 1024

#define TF_MAX_ID_LST 10
#define TF_MAX_TYPE_LST 10
#define TF_MAX_GEN_LST 5

// Ticks (TF_Tick() calls) before an incomplete frame is abandoned.
#define TF_PARSER_TIMEOUT_TICKS 10

// Single-threaded on both MCUs (one loop() owns the link) — no TX mutex.
#define TF_USE_MUTEX 0

#define TF_Error(format, ...) printf("[TF] " format "\n", ##__VA_ARGS__)

//------------------------- End of user config ------------------------------

#endif // TF_CONFIG_H
