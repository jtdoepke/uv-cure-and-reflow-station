// CydLink — the CYD's reliability facade (design.md §9). Composes the three
// CYD-role pieces (Handshake, HeartbeatSender, ReliableSender) behind one
// IMessageObserver + one service(). The firmware (A8) constructs this, points a
// MessageRouter at it, and calls service() every loop; tests can drive the
// pieces directly instead.
//
// Content messages from the controller (Telemetry/Done/Fault) are forwarded to
// an optional app observer so Track B can consume them without CydLink knowing
// their meaning. Header-only: thin composition, no state beyond its members.
//
// It does read one thing out of that stream on its own account: *arrival*. Telemetry is the
// only traffic the controller sends unprompted (§9, 250 ms, run or no run), so its freshness is
// the CYD's sole evidence the controller is still there — matched() only ever proves one
// answered a Hello once, and latches. linkAlive() is that evidence, and it is what Home's link
// indicator + run-flow gate (§14) and B7's FaultController.linkHealthy (§22) key off.
#pragma once

#include "IClock.h"
#include "handshake.h"
#include "heartbeat_monitor.h"
#include "heartbeat_sender.h"
#include "link_params.h"
#include "message_router.h"
#include "oven.pb.h"
#include "reliable_sender.h"

namespace protocol {

class CydLink : public IMessageObserver {
public:
  CydLink(FrameLink &link, IClock &clock)
      : handshake_(link, clock), heartbeat_(link, clock), sender_(link, clock), telemetry_(clock) {}

  // Send our Hello at boot. boot_nonce must differ across boots of this board — it is
  // what lets the controller notice a CYD restart and re-announce itself (§9 re-sync).
  void begin(uint32_t boot_nonce) { handshake_.begin(boot_nonce); }

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

  // Is the controller still there? True only while its telemetry keeps arriving (§9: it sends
  // unconditionally at kTelemetryPeriodMs, run or no run).
  //
  // This — not handshake().matched() — is the liveness signal. matched() answers "did a peer
  // once answer a Hello, and did we agree on the .proto", and it *latches*: it stays true over
  // a cable that came out ten minutes ago. Telemetry arrival is the only thing that decays.
  // Fail-closed: false until the first frame lands.
  bool linkAlive() const { return !telemetry_.expired(kLinkTimeoutMs); }

  // The most recent telemetry frame, and whether one has ever arrived. The CYD reads this each
  // loop to drive Home's live chamber temp + run-state badge (§14) — the same "CydLink reads the
  // stream on its own account" posture as linkAlive(), so the single app observer (the
  // ManagementClient) stays free for request/reply correlation. Pair with linkAlive() before
  // trusting the contents: a stale frame past the timeout is no longer the machine's real state.
  const oven_Telemetry &lastTelemetry() const { return last_telemetry_; }
  bool hasTelemetry() const { return have_telemetry_; }

  // IMessageObserver — reliability messages handled here; content forwarded.
  void onHello(const oven_Hello &h) override { handshake_.onPeerHello(h); }
  void onAck(const oven_Ack &a) override { sender_.onAck(a); }
  void onNak(const oven_Nak &n) override { sender_.onNak(n); }
  void onTelemetry(const oven_Telemetry &t) override {
    telemetry_.feed(); // arrival is the liveness signal, whatever the frame says
    last_telemetry_ = t;
    have_telemetry_ = true;
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

  // Profile/settings management replies (design.md §9; Wave R3) — forwarded to the app observer
  // (the CYD's ManagementClient), which correlates them to its outstanding request. CydLink stays
  // ignorant of their meaning, exactly as with Telemetry/Done/Fault.
  void onProfileList(const oven_ProfileList &m) override {
    if (app_ != nullptr) {
      app_->onProfileList(m);
    }
  }
  void onProfileData(const oven_ProfileData &m) override {
    if (app_ != nullptr) {
      app_->onProfileData(m);
    }
  }
  void onSettingsData(const oven_SettingsData &m) override {
    if (app_ != nullptr) {
      app_->onSettingsData(m);
    }
  }
  void onMgmtResult(const oven_MgmtResult &m) override {
    if (app_ != nullptr) {
      app_->onMgmtResult(m);
    }
  }

private:
  Handshake handshake_;
  HeartbeatSender heartbeat_;
  ReliableSender sender_;
  HeartbeatMonitor telemetry_; // freshness of the controller's stream, not of a heartbeat
  oven_Telemetry last_telemetry_ = oven_Telemetry_init_zero; // last payload, for Home's live values
  bool have_telemetry_ = false;
  IMessageObserver *app_ = nullptr;
};

} // namespace protocol
