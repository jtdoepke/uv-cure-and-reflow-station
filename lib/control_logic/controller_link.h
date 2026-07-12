// ControllerLink — the controller's reliability facade (design.md §9). Composes
// the controller-role pieces (Handshake, SetupResponder, SessionGate) behind one
// IMessageObserver + one service(), and exposes authorized() for the
// SafetySupervisor (A4) to poll. Accepted Starts adopt their session into the
// gate (via ISetupSink); Abort clears it.
//
// Validation is delegated: pass an ISetupValidator to plug in A7's real checks,
// or omit it to accept everything (A2's default). Lives in control_logic because
// it owns a SessionGate. Header-only: thin composition.
#pragma once

#include <cstdint>

#include "IClock.h"
#include "frame_link.h"
#include "handshake.h"
#include "message_router.h"
#include "oven.pb.h"
#include "session_gate.h"
#include "setup_responder.h"

class ControllerLink : public protocol::IMessageObserver, public protocol::ISetupSink {
public:
  // link/clock (and validator, if given) must outlive this. fw_ver/caps ride in
  // our Hello. validator == nullptr uses the built-in accept-all default.
  ControllerLink(protocol::FrameLink &link, IClock &clock,
                 protocol::ISetupValidator *validator = nullptr, uint32_t fw_ver = 0,
                 uint32_t caps = 0)
      : validator_(validator != nullptr ? *validator : default_validator_),
        handshake_(link, clock, fw_ver, caps), responder_(link, validator_),
        gate_(clock, handshake_) {
    responder_.setSink(*this);
  }

  // Send our Hello at boot.
  void begin() { handshake_.sendHello(); }

  // Retransmit Hello until the peer is seen. Call every loop iteration.
  void service() { handshake_.service(); }

  // The composed run authorization (design.md §9) — what A4 gates outputs on.
  bool authorized() const { return gate_.authorized(); }

  protocol::Handshake &handshake() { return handshake_; }
  SessionGate &gate() { return gate_; }

  // IMessageObserver — route each message to the piece that owns it.
  void onHello(const oven_Hello &h) override { handshake_.onPeerHello(h); }
  void onRecipe(const oven_Recipe &r) override { responder_.onRecipe(r); }
  void onStart(const oven_Start &s) override { responder_.onStart(s); }
  void onHeartbeat(const oven_Heartbeat &hb) override { gate_.onHeartbeat(hb); }
  void onAbort() override { gate_.clearSession(); }

  // ISetupSink — an accepted Start authorizes its session.
  void onStartAccepted(const oven_Start &s) override { gate_.adoptSession(s.session); }

private:
  protocol::AcceptAllValidator default_validator_;
  protocol::ISetupValidator &validator_;
  protocol::Handshake handshake_;
  protocol::SetupResponder responder_;
  SessionGate gate_;
};
