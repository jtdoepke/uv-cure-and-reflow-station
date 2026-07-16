// Handshake — the schema-hash gate, fail-closed (design.md §9).
//
// At boot each board sends Hello{proto_ver, schema_hash, boot_nonce}; the controller
// also fills fw_ver/caps. Handshake retransmits Hello until it has seen the peer's
// (covering the race where one board powers up first), and records whether the
// peer's proto_ver + schema_hash match ours. matched() is false until a matching
// Hello arrives and flips back to false if a later Hello (e.g. a rebooted peer
// built against a different .proto) no longer matches — so a version-skewed pair
// can never authorize a run. On the controller matched() is one required term of
// SessionGate::authorized(); on the CYD a false reading drives the
// "controller/UI schema mismatch" error.
//
// Re-sync after a peer reboot (§9): announcing stops once the peer is seen, so a
// board that reboots *after* the handshake would send Hellos into silence and never
// match again — the controller would sit at matched()==false refusing to leave safe
// state, which would make every watchdog reset (§11) take the link down with it. So
// a Hello bearing a *different* boot_nonce than the one we last saw is read as "the
// peer restarted and knows nothing about us" and answered with our own Hello. Only a
// changed nonce triggers that answer: our reply carries our unchanged nonce, so the
// peer will not answer it back, and two live boards can never trade Hellos forever.
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

  // Adopt this boot's nonce and send our first Hello. Call once at boot.
  //
  // The nonce is a begin() argument rather than a constructor one for two reasons: the
  // firmware's link objects are file-scope statics, so their constructors run before a
  // random source is usable; and begin() is already mandatory, so a required parameter
  // here cannot be forgotten. That matters — a nonce left at its default would make
  // every peer look unchanged, silently restoring the no-re-sync bug this exists to
  // fix. It must differ across boots of the same board; anything else is a policy the
  // caller owns (the firmware rolls esp_random(); tests pass literals).
  void begin(uint32_t boot_nonce);

  // Send our Hello now and restart the retransmit timer.
  void sendHello();

  // Retransmit Hello every kHelloRetryMs until the peer's Hello is seen. Call
  // every loop iteration; a no-op once the peer has answered.
  void service();

  // Feed a decoded peer Hello (from MessageRouter::onHello). Updates matched_.
  // Returns true when this Hello is from a peer we had not seen or one whose
  // boot_nonce changed (i.e. the peer just (re)started) — the signal a caller uses
  // to drop any per-peer state that a reboot invalidates (e.g. the setup dedup cache).
  bool onPeerHello(const oven_Hello &hello);

  // True once a peer Hello has been seen whose proto_ver + schema_hash equal
  // ours. Fail-closed: false until then, and false again after a mismatching
  // Hello.
  bool matched() const { return matched_; }

  // True once any peer Hello (matching or not) has been received.
  bool sawPeer() const { return saw_peer_; }

  // The most recent peer Hello (zeroed until sawPeer()).
  const oven_Hello &peer() const { return peer_; }

  // This boot's nonce, as sent in our Hello. Diagnostics only.
  uint32_t bootNonce() const { return boot_nonce_; }

private:
  FrameLink &link_;
  IClock &clock_;
  uint32_t fw_ver_;
  uint32_t caps_;
  uint32_t boot_nonce_ = 0; // set by begin()

  oven_Hello peer_ = oven_Hello_init_default;
  uint32_t last_send_ms_ = 0;
  bool sent_ = false;
  bool saw_peer_ = false;
  bool matched_ = false;
};

} // namespace protocol
