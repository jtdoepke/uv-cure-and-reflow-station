// FrameLink — the TinyFrame framing endpoint over an ISerialTransport port
// (design.md §9). One instance per peer (CYD or controller); the same code runs
// over the real UART in firmware and an in-memory pipe in host tests.
//
// TinyFrame is plain C: it emits bytes through the single program-wide hook
// TF_WriteImpl() (defined once in frame_link.cpp) and delivers received frames
// through listener callbacks. Both reach a TinyFrame instance only via its
// `tf.userdata`, so we stash the owning FrameLink there — TF_WriteImpl finds the
// transport to write to, and the generic listener finds the handler to dispatch
// to. That also lets two peers share one process (each TinyFrame carries its own
// userdata), which is exactly how the round-trip tests wire a pipe together.
//
// This is A1: pure framing + generic dispatch. It hands received frames up as
// raw (type, payload) pairs and knows nothing about their meaning. Seq/ACK-NAK
// retry, heartbeat liveness, session and the schema-hash handshake gate are the
// A2 layer built on top.
//
// Keep this header free of <Arduino.h> so lib/protocol stays native-compilable.
#pragma once

#include <cstddef>
#include <cstdint>

#include "ISerialTransport.h"

extern "C" {
#include "TinyFrame/TinyFrame.h" // no C++ guards upstream; C linkage matches TinyFrame.c
}

namespace protocol {

// Sink for fully-parsed, CRC-valid received frames. A1 delivers raw payload
// bytes only; decoding (protocol::decode) and semantics are the caller's job.
// The payload pointer is owned by TinyFrame and valid only for the duration of
// the call — copy or decode before returning.
struct IFrameHandler {
  virtual ~IFrameHandler() = default;
  virtual void onFrame(uint8_t type, const uint8_t *payload, size_t len) = 0;
};

class FrameLink {
public:
  // peer = TF_MASTER (CYD) or TF_SLAVE (controller): opposite peers so their
  // generated frame ids don't collide once A2 adds seq/ACK. transport and
  // handler must outlive this FrameLink.
  FrameLink(ISerialTransport &transport, TF_Peer peer, IFrameHandler &handler);

  FrameLink(const FrameLink &) = delete;
  FrameLink &operator=(const FrameLink &) = delete;

  // Frame payload as a TinyFrame frame of the given type and emit it through the
  // transport. Returns false if TinyFrame rejects it (e.g. payload too large).
  bool send(uint8_t type, const uint8_t *payload, size_t len);

  // Drain everything currently readable from the transport into the parser,
  // dispatching any complete frames to the handler. Call every loop iteration.
  void poll();

  // Advance TinyFrame's tick clock once (parser resync-on-timeout, §9). Call at
  // a steady cadence; the timeout is measured in these ticks, not wall time.
  void tick();

  // Exposed so the single global TF_WriteImpl can reach the bound transport.
  ISerialTransport &transport() { return transport_; }

private:
  static TF_Result onGeneric(TinyFrame *tf, TF_Msg *msg);

  TinyFrame tf_{};
  ISerialTransport &transport_;
  IFrameHandler &handler_;
};

} // namespace protocol
