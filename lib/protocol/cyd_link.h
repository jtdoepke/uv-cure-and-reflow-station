// CydLink — the CYD's reliability facade (design.md §9). Composes the three
// CYD-role pieces (Handshake, HeartbeatSender, ReliableSender) behind one
// IMessageObserver + one service(). The firmware (A8) constructs this, points a
// MessageRouter at it, and calls service() every loop; tests can drive the
// pieces directly instead.
//
// Content messages from the controller (Telemetry/Done/Fault) are forwarded to
// an optional app observer so Track B can consume them without CydLink knowing
// their meaning. Header-only: thin composition, no state beyond its members.
#pragma once

#include "handshake.h"
#include "heartbeat_sender.h"
#include "message_router.h"
#include "reliable_sender.h"

namespace protocol {

class CydLink : public IMessageObserver {
public:
  CydLink(FrameLink &link, IClock &clock)
      : handshake_(link, clock), heartbeat_(link, clock), sender_(link, clock) {}

  // Send our Hello at boot.
  void begin() { handshake_.sendHello(); }

  // Drive all three cadences. Call every loop iteration.
  void service() {
    handshake_.service();
    heartbeat_.service();
    sender_.service();
  }

  // Optional sink for controller->CYD content messages (Track B).
  void setAppObserver(IMessageObserver &app) { app_ = &app; }

  Handshake &handshake() { return handshake_; }
  HeartbeatSender &heartbeat() { return heartbeat_; }
  ReliableSender &sender() { return sender_; }

  // IMessageObserver — reliability messages handled here; content forwarded.
  void onHello(const oven_Hello &h) override { handshake_.onPeerHello(h); }
  void onAck(const oven_Ack &a) override { sender_.onAck(a); }
  void onNak(const oven_Nak &n) override { sender_.onNak(n); }
  void onTelemetry(const oven_Telemetry &t) override {
    if (app_ != nullptr) {
      app_->onTelemetry(t);
    }
  }
  void onDone(const oven_Done &d) override {
    if (app_ != nullptr) {
      app_->onDone(d);
    }
  }
  void onFault(const oven_Fault &f) override {
    if (app_ != nullptr) {
      app_->onFault(f);
    }
  }

private:
  Handshake handshake_;
  HeartbeatSender heartbeat_;
  ReliableSender sender_;
  IMessageObserver *app_ = nullptr;
};

} // namespace protocol
