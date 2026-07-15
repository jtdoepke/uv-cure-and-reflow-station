// control_board.h — the controller's link-UART and console policy (design.md §2, §25).
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
