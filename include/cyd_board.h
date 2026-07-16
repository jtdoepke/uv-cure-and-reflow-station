// cyd_board.h — the HMI board's wiring, capabilities and orientation, in one place.
//
// The CYD-side twin of src_control/control_board.h, and deliberately written to read like it:
// main.cpp should carry no pin literal, and which board (and which way up) we build for should be
// a build flag rather than a patch. Same shape, same reasons — see that file's header.
//
// Unlike its twin this lives in include/ rather than src_cyd/, because test/test_embedded_hw
// needs the geometry to assert against: a PlatformIO test env does not compile src/
// (test_build_src defaults to no) but include/ is always on the path. Putting `-I src_cyd` on the
// embedded env would buy the symmetry back at the cost of dropping every Arduino adapter onto
// that env's include path.
//
// Two flag families, two jobs, deliberately separate:
//   CYD_BOARD_*  picks the pin map + panel driver. Read here and nowhere else; nothing under
//                lib/ may ever branch on a board identity.
//   PANEL_*      is the geometry (panel.h). Shared with lib/ui_logic, sim/ and the native envs,
//                which have no board at all. Both come from ONE [board_*] section in
//                platformio.ini, so they cannot disagree — and the static_assert below proves it
//                rather than trusting it.
#pragma once

#include <Arduino.h>

#include "panel.h"

// `extends` in platformio.ini REPLACES a redeclared key rather than merging it, and the tempting
// fix — build_flags = ${env:esp32dev_cyd.build_flags} ${board_3248s035.build_flags} — composes
// BOTH board sections. The #if below would then silently pick whichever branch comes first and
// produce a clean firmware with the wrong pin map. Fail the build instead. (This is why the board
// envs compose [cyd_common] + exactly one [board_*] and never extend each other.)
#if defined(CYD_BOARD_2432S028) && defined(CYD_BOARD_3248S035)
#error "Two CYD boards selected - an env is composing both [board_*] sections. See platformio.ini."
#endif

#if defined(CYD_BOARD_3248S035)
#include "LGFX_CYD3248S035.hpp"
static_assert(panel::kNativeW == 320 && panel::kNativeH == 480,
              "PANEL_* geometry does not match CYD_BOARD_3248S035 - the env is mixing sections");
#elif defined(CYD_BOARD_2432S028)
#include "LGFX_CYD2432S028.hpp"
static_assert(panel::kNativeW == 240 && panel::kNativeH == 320,
              "PANEL_* geometry does not match CYD_BOARD_2432S028 - the env is mixing sections");
#else
#error "No CYD board selected - define CYD_BOARD_2432S028 or CYD_BOARD_3248S035 ([board_*])"
#endif

// Orientation. Applied as a runtime setRotation() rather than a compile-time panel-cfg edit,
// because that is exactly what LovyanGFX is built for: Panel_LCD::setRotation composes r with the
// panel's fixed cfg.offset_rotation, and Panel_Device::convertRawXY composes the SAME r with the
// touch's fixed cfg.offset_rotation. Both offset_rotations are therefore per-board CALIBRATION
// constants, and touch tracks the display through every rotation with no per-rotation math on our
// side. (The 2.8" board's display-1/touch-2 pair is not a relationship to maintain; 2 is simply
// that board's constant.)
inline constexpr int kRotation = panel::kRotation;

// --- Link UART (§9) ---
// Serial1 with an explicit pin remap: its defaults (GPIO9/10) are wired to the SPI flash, so the
// 4-arg begin() is mandatory, not stylistic. Using Serial1 also leaves "Serial2" meaning its
// stock 16/17, as it does in the controller's bench build. §2 keeps the link clear of the CYD's
// own UART0/USB on every board, so this side needs no bench flag (the controller's does).
inline HardwareSerial &linkSerial() {
  return Serial1;
}
#if defined(CYD_BOARD_3248S035)
// Free precisely because this board's touch shares the display's SPI bus: 25/32 are the 2.8"'s
// bit-banged touch SCLK/MOSI. Its link pins are unavailable here — 27 is the backlight.
inline constexpr int kLinkRxPin = 25;
inline constexpr int kLinkTxPin = 32;
#else
inline constexpr int kLinkRxPin = 22; // CN1 header (§2)
inline constexpr int kLinkTxPin = 27;
#endif
inline constexpr uint32_t kLinkBaud = 115200; // §9: 115200 8N1

// TinyFrame's parser-resync timeout is counted in TF_Tick() *calls*, not milliseconds
// (TF_PARSER_TIMEOUT_TICKS = 10, TF_Config.h), so ticking on a fixed 10 ms cadence turns it into a
// real ~100 ms — comfortably inside the controller's 750 ms command-timeout.
inline constexpr uint32_t kLinkTickMs = 10;
inline constexpr size_t kLinkRxBuf = 512;  // we receive Telemetry/Ack — modest
inline constexpr size_t kLinkTxBuf = 2048; // we SEND Recipes: must exceed TF_SENDBUF_LEN (1024)

// --- Ambient light (§18) ---
// A capability expressed as data, not as an #if: the Settings screen that shows the
// auto-brightness row lives in lib/ui_logic and must never see a board flag. A board without an
// LDR gets a null adapter and a disabled AutoBrightness — one firmware shape, no dead branches,
// and the constant folds away.
// A macro as well as the constant, because the two uses genuinely differ: main.cpp must pick
// *which adapter type to instantiate* (a preprocessor job — Esp32AmbientLight takes a pin,
// NullAmbientLight takes nothing), while everything else just reads the value. One source, the
// constant derived from it, so they cannot disagree.
#if defined(CYD_BOARD_3248S035)
#define CYD_HAS_AMBIENT_LIGHT 0 // verified on the bench: no LDR fitted to this board
inline constexpr int kAmbientPin = -1;
#else
#define CYD_HAS_AMBIENT_LIGHT 1
inline constexpr int kAmbientPin = 34; // on-board LDR, ADC1 (reads fine with WiFi on)
#endif
inline constexpr bool kHasAmbientLight = CYD_HAS_AMBIENT_LIGHT;
inline constexpr adc_attenuation_t kAmbientAtten = ADC_11db; // full ~0-3.3 V LDR swing (§18)

// --- GRAM readback ---
// Whether the panel can be read back, i.e. whether its SDO is wired. Mirrors cfg.readable in the
// board's LGFX header; kept here because UI_DEV_TOOLS' /screenshot.bmp is the only thing that
// depends on it, and it must refuse rather than serve a screenshot of nothing. On the 3.5" board
// readback returns all zeros, so dev-shot silently wrote a perfectly black 320x480 PNG — a
// convincing lie, which is the failure mode worth spending a constant to avoid.
#if defined(CYD_BOARD_3248S035)
inline constexpr bool kPanelReadable = false;
#else
inline constexpr bool kPanelReadable = true;
#endif

// --- LVGL draw buffers ---
// No PSRAM on any CYD, so partial buffers only — sized in SCANLINES rather than as a fraction of
// the screen. The fraction was the wrong invariant: 1/10 of a 320x480 panel is 30720 B per
// buffer, DOUBLE the 2.8"'s, and x2 for double-buffering that is ~61 KB of DRAM on a board whose
// WiFi build already sits near the malloc cliff. Lines hold both the DMA chunk size and the DRAM
// cost constant across panels; a denser panel simply takes more chunks per full refresh, which
// costs approximately nothing on a UI that redraws small dirty regions.
#ifndef DRAW_BUF_LINES
#define DRAW_BUF_LINES 24 // 320 x 24 x 2 B = 15360 B/buffer — the 2.8" board's long-standing size
#endif
inline constexpr size_t kDrawBufBytes = static_cast<size_t>(panel::W) * DRAW_BUF_LINES * 2;

// Double-buffered async DMA flush. -D DISP_DOUBLE_BUFFER=0 reclaims kDrawBufBytes: it drops the
// second draw buffer and reverts to the single-buffer, CPU-blocking flush, at the cost of display
// responsiveness. Turn this off FIRST if a WiFi build starts failing the draw-buffer malloc — it
// is the easiest chunk of DRAM to give back. A flag rather than a source #define so an env can
// make that trade without a patch.
#ifndef DISP_DOUBLE_BUFFER
#define DISP_DOUBLE_BUFFER 1
#endif
