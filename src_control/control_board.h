// control_board.h — the controller's wiring, timing and console policy (design.md §2, §6, §25).
//
// Every pin and hard-coded timeout the controller firmware owns is defined here, so main.cpp is a
// composition root rather than a board definition and §6's pin inventory has one place to be true.
// Be honest about the asymmetry with its CYD twin (include/cyd_board.h): that file exists because
// there are genuinely two HMI boards. There is only one controller, and this file is NOT variant
// support — it is consolidation, for §6 traceability and for the day the custom PCB moves a pin.
// Do not grow a [board_*] mechanism here on spec; CONTROL_BENCH below is a *console* choice that
// happens to move a UART, which is a different thing.
//
// design.md §2 pins the production link to UART0 (GPIO1/3): those are the pins the ESP32's ROM
// serial loader speaks, which §25's CYD-driven reflash needs. On a dev board UART0 is *also*
// the USB-serial bridge's port, so the CYD's TX and the bridge's TX both drive the controller's
// RX0 — and a powered bridge idles its TX *driven high*, winning every arbitration. You
// therefore cannot have both boards on USB and the link up at once. CONTROL_BENCH (env
// esp32dev_control_bench) moves the link to UART2 (GPIO16/17) for the §8 step-1 bench proof and
// hands UART0 back to USB flash + monitor. The CYD needs no such flag: §2 already put its link
// on GPIO27/22, clear of its own USB.
//
// One flag, not two, because the UART choice and the console choice are the same decision: in
// production the link OWNS Serial, so a boot banner or a stray Serial.printf is bytes on the
// wire. CONTROL_LOGF compiles to nothing there — deliberately, not by omission. TF_Config.h
// silences TinyFrame's own TF_Error printf for exactly the same reason (TF_ERROR_QUIET).
#pragma once

#include <Arduino.h>

#if defined(CONTROL_BENCH)

// Bench: link on UART2 (Serial2's stock pins), console on UART0/USB.
inline HardwareSerial &linkSerial() {
  return Serial2;
}
inline constexpr int kLinkRxPin = 16;
inline constexpr int kLinkTxPin = 17;
#define CONTROL_LOGF(...) Serial.printf(__VA_ARGS__)

#else

// Production: the link owns UART0 — the ROM loader's pins (§2, §25). No console exists.
inline HardwareSerial &linkSerial() {
  return Serial;
}
inline constexpr int kLinkRxPin = 3; // RX0
inline constexpr int kLinkTxPin = 1; // TX0
#define CONTROL_LOGF(...) ((void)0)

#endif

inline constexpr uint32_t kLinkBaud = 115200; // §9: 115200 8N1

// TinyFrame emits a whole frame in one TF_WriteImpl call and ignores the byte count
// (lib/protocol/frame_link.cpp), so a short write truncates a frame mid-flight with no resume
// path. Sizing the TX ring above TF_SENDBUF_LEN (1024, TF_Config.h) keeps HardwareSerial::write
// a buffered copy that never comes up short. RX covers a full Recipe landing between poll()s.
inline constexpr size_t kLinkTxBuf = 2048;
inline constexpr size_t kLinkRxBuf = 1024;

// The tick cadence is protocol::kLinkTickMs (lib/protocol/link_params.h) — a protocol fact shared
// with the CYD, not a board one.

// --- Outputs (§4, §6) ---
// Both are plain outputs: not strapping pins (0/2/5/12/15), not input-only (34-39). Both are
// pulled DOWN in hardware, which is what makes a crashed, reset, brown-out or bootloader-stuck MCU
// safe with no firmware action; the adapters' begin() is only the software half of that default.
// On the bench (A8, §8 step 1) each drives an LED + series resistor instead of the real load.
inline constexpr int kHeaterPin = 25;    // zero-cross SSR gate
inline constexpr int kContactorPin = 26; // mains-isolation contactor coil driver

// --- Watchdog (§9) ---
// Comfortably longer than a worst-case controller loop (LVGL lives on the other MCU; ours is link
// + control only), short enough that a hang is caught long before it could matter thermally. A4b
// may tighten this once the real loop's timing is measured.
inline constexpr uint32_t kWatchdogTimeoutMs = 5000;
