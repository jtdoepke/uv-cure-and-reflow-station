// FlakyTransport — an ISerialTransport decorator that can drop outbound frames,
// for exercising the setup path's seq/ACK retry (design.md §9). The existing
// LoopbackPipe delivers everything reliably; wrap one of its endpoints and set
// drop_tx to swallow whatever a class writes during a chosen window, then clear
// it to let a later retransmit through.
//
// Because the native_control process is single-threaded and TinyFrame emits a
// frame's bytes synchronously inside send(), toggling drop_tx around a send/retry
// gives frame-granular loss without any per-frame bookkeeping.
//
// Host-only; keep it out of lib/ (mirrors pipe_transport.h).
#pragma once

#include <cstddef>
#include <cstdint>

#include "ISerialTransport.h"

class FlakyTransport : public ISerialTransport {
public:
  explicit FlakyTransport(ISerialTransport &inner) : inner_(inner) {}

  // While true, write() reports success but discards the bytes (a lost frame).
  bool drop_tx = false;

  size_t write(const uint8_t *data, size_t len) override {
    if (drop_tx) {
      return len; // pretend the whole frame went out; it never reaches the wire
    }
    return inner_.write(data, len);
  }

  size_t read(uint8_t *buf, size_t len) override { return inner_.read(buf, len); }

private:
  ISerialTransport &inner_;
};
