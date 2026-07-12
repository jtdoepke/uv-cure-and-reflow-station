// ISerialTransport — non-blocking byte pipe under the CYD<->controller link
// (design.md §9). lib/protocol's framing (TinyFrame) and everything above it
// talk to this port; the production adapter wraps the real UART, tests use an
// in-memory pipe. Both write() and read() are best-effort and never block.
//
// Keep this header free of <Arduino.h> so it stays native-compilable.
#pragma once

#include <cstddef>
#include <cstdint>

struct ISerialTransport {
  virtual ~ISerialTransport() = default;
  // Queue up to len bytes for transmission; returns how many were accepted.
  virtual size_t write(const uint8_t *data, size_t len) = 0;
  // Drain up to len received bytes into buf; returns how many were available.
  virtual size_t read(uint8_t *buf, size_t len) = 0;
};
