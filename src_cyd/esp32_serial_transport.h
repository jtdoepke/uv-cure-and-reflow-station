// Esp32SerialTransport — the ISerialTransport firmware adapter (design.md §9).
//
// Wraps a HardwareSerial as TinyFrame's byte pipe. Firmware-only: host tests drive FrameLink
// through LoopbackPipe (test/helpers/pipe_transport.h) instead, which is why the framing and
// everything above it stay testable with no board.
//
// write() deliberately does NOT clamp to availableForWrite(). TinyFrame emits a whole frame in
// a single TF_WriteImpl call and *ignores the returned count* (lib/protocol/frame_link.cpp), so
// there is no resume path — a short write silently truncates a frame, and the peer sees a CRC
// failure whose cause is nowhere near the symptom. The port's "never block" contract is met
// instead by sizing the UART's TX ring above TF_SENDBUF_LEN before begin(), which for the CYD
// matters more than for the controller: this is the side that *sends* Recipes.
//
// src_control/esp32_serial_transport.h is a deliberate twin — see esp32_clock.h on why the
// adapters are duplicated per firmware tree rather than shared via lib/.
//
// The HardwareSerial is held by reference and must outlive this adapter.
#pragma once

#include <Arduino.h>

#include "ISerialTransport.h"

class Esp32SerialTransport : public ISerialTransport {
public:
  explicit Esp32SerialTransport(HardwareSerial &uart) : uart_(uart) {}

  size_t write(const uint8_t *data, size_t len) override { return uart_.write(data, len); }

  // Never blocks: readBytes()'s timeout cannot fire because we ask for at most what has
  // already arrived.
  size_t read(uint8_t *buf, size_t len) override {
    const int avail = uart_.available();
    if (avail <= 0) {
      return 0;
    }
    const size_t n = static_cast<size_t>(avail) < len ? static_cast<size_t>(avail) : len;
    return uart_.readBytes(buf, n);
  }

private:
  HardwareSerial &uart_;
};
