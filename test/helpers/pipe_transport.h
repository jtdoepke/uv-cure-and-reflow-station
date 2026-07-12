// LoopbackPipe — an in-memory ISerialTransport pair for host tests (design.md
// §9 names an "in-memory pipe" as the transport test double). Two endpoints are
// cross-wired: whatever endpoint A writes, endpoint B reads, and vice versa —
// so two FrameLinks can talk to each other inside one native_control process
// without a real UART.
//
// Host-only (std::deque), never compiled into firmware. Keep it out of lib/.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>

#include "ISerialTransport.h"

// One direction of the pipe: bytes written on one side, read from the other.
class ByteFifo {
public:
  size_t push(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
      buf_.push_back(data[i]);
    }
    return len; // unbounded in tests: every byte is always accepted
  }
  size_t pop(uint8_t *out, size_t len) {
    size_t n = 0;
    while (n < len && !buf_.empty()) {
      out[n++] = buf_.front();
      buf_.pop_front();
    }
    return n;
  }
  size_t size() const { return buf_.size(); }

private:
  std::deque<uint8_t> buf_;
};

class LoopbackPipe {
public:
  // Endpoint reading one FIFO and writing the other. Constructed by LoopbackPipe.
  class Endpoint : public ISerialTransport {
  public:
    Endpoint(ByteFifo &tx, ByteFifo &rx) : tx_(tx), rx_(rx) {}
    size_t write(const uint8_t *data, size_t len) override { return tx_.push(data, len); }
    size_t read(uint8_t *buf, size_t len) override { return rx_.pop(buf, len); }

  private:
    ByteFifo &tx_;
    ByteFifo &rx_;
  };

  LoopbackPipe() : a_(a_to_b_, b_to_a_), b_(b_to_a_, a_to_b_) {}

  ISerialTransport &a() { return a_; }
  ISerialTransport &b() { return b_; }

  // Test hooks: inject raw bytes straight onto a wire (e.g. a corrupted frame),
  // bypassing an endpoint's write() — used to exercise the parser's error paths.
  void injectAToB(const uint8_t *data, size_t len) { a_to_b_.push(data, len); }
  void injectBToA(const uint8_t *data, size_t len) { b_to_a_.push(data, len); }

  // Bytes still queued in each direction (for assertions / draining checks).
  size_t pendingAToB() const { return a_to_b_.size(); }
  size_t pendingBToA() const { return b_to_a_.size(); }

private:
  ByteFifo a_to_b_;
  ByteFifo b_to_a_;
  Endpoint a_;
  Endpoint b_;
};
