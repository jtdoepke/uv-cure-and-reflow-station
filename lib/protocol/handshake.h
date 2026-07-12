// Handshake — the schema-hash gate, fail-closed (design.md §9).
//
// At boot each board sends Hello{proto_ver, schema_hash}; the controller also
// fills fw_ver/caps. Handshake retransmits Hello until it has seen the peer's
// (covering the race where one board powers up first), and records whether the
// peer's proto_ver + schema_hash match ours. matched() is false until a matching
// Hello arrives and flips back to false if a later Hello (e.g. a rebooted peer
// built against a different .proto) no longer matches — so a version-skewed pair
// can never authorize a run. On the controller matched() is one required term of
// SessionGate::authorized(); on the CYD a false reading drives the
// "controller/UI schema mismatch" error.
//
// Symmetric: the same class runs on both sides. Reads the clock internally in
// service() (same idiom as HeartbeatMonitor). Keep free of <Arduino.h>.
#pragma once

#include <cstdint>

#include "IClock.h"
#include "frame_link.h"
#include "messages.h"

namespace protocol {

class Handshake {
public:
  // link and clock must outlive this Handshake. fw_ver/caps are sent in our
  // Hello (meaningful controller->CYD only; the CYD passes 0). The local
  // proto_ver/schema_hash come from schema.h.
  Handshake(FrameLink &link, IClock &clock, uint32_t fw_ver = 0, uint32_t caps = 0)
      : link_(link), clock_(clock), fw_ver_(fw_ver), caps_(caps) {}

  // Send our Hello now and start the retransmit timer. Call once at boot.
  void sendHello();

  // Retransmit Hello every kHelloRetryMs until the peer's Hello is seen. Call
  // every loop iteration; a no-op once the peer has answered.
  void service();

  // Feed a decoded peer Hello (from MessageRouter::onHello). Updates matched_.
  void onPeerHello(const oven_Hello &hello);

  // True once a peer Hello has been seen whose proto_ver + schema_hash equal
  // ours. Fail-closed: false until then, and false again after a mismatching
  // Hello.
  bool matched() const { return matched_; }

  // True once any peer Hello (matching or not) has been received.
  bool sawPeer() const { return saw_peer_; }

  // The most recent peer Hello (zeroed until sawPeer()).
  const oven_Hello &peer() const { return peer_; }

private:
  FrameLink &link_;
  IClock &clock_;
  uint32_t fw_ver_;
  uint32_t caps_;

  oven_Hello peer_ = oven_Hello_init_default;
  uint32_t last_send_ms_ = 0;
  bool sent_ = false;
  bool saw_peer_ = false;
  bool matched_ = false;
};

} // namespace protocol
