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
#include "management_responder.h"
#include "message_router.h"
#include "oven.pb.h"
#include "oven_safety.h"
#include "profile_executor.h"
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

  // Optionally drive a profile executor (A6) off the accepted setup path: a validated
  // Recipe is load()ed, a matching Start start()s it, an Abort abort()s it. Optional so
  // the many tests that build a bare ControllerLink (and the not-yet-wired main loop that
  // has no temp adapter) keep working; the executor is ticked elsewhere (it needs the
  // measured control temp, which the link never sees). Must outlive this link.
  void setExecutor(ProfileExecutor &executor) { executor_ = &executor; }

  // Optionally attach the profile/settings management responder (design.md §9; Wave R2). Optional
  // so the many tests that build a bare ControllerLink keep working; the responder needs the
  // per-mode stores, which are firmware wiring. When attached, the profile-management request
  // frames route to it and a peer reboot clears its dedup cache alongside the setup responder's.
  void setManagementResponder(ManagementResponder &mgmt) { mgmt_ = &mgmt; }

  // Send our Hello at boot. boot_nonce must differ across boots of this board — it is
  // what lets the CYD notice a controller restart (watchdog, brownout, crash) and
  // re-announce itself, without which we would never re-match and would sit refusing
  // to leave safe state (§9 re-sync).
  void begin(uint32_t boot_nonce) { handshake_.begin(boot_nonce); }

  // Retransmit Hello until the peer is seen. Call every loop iteration.
  void service() { handshake_.service(); }

  // The composed run authorization (design.md §9) — what A4 gates outputs on.
  bool authorized() const { return gate_.authorized(); }

  protocol::Handshake &handshake() { return handshake_; }
  SessionGate &gate() { return gate_; }

  // IMessageObserver — route each message to the piece that owns it. A peer that
  // just (re)booted invalidates the setup dedup cache: its ReliableSender re-seeded
  // its seq, so an old cached verdict must not shadow the new run's first command.
  void onHello(const oven_Hello &h) override {
    if (handshake_.onPeerHello(h)) {
      responder_.reset();
      if (mgmt_ != nullptr) {
        mgmt_->reset();
      }
    }
  }
  void onRecipe(const oven_Recipe &r) override { responder_.onRecipe(r); }
  void onStart(const oven_Start &s) override { responder_.onStart(s); }
  void onHeartbeat(const oven_Heartbeat &hb) override { gate_.onHeartbeat(hb); }

  // Profile management (design.md §9) — forwarded to the responder when one is attached. The
  // router dispatches through this single observer (ControllerLink), so it relays the request
  // frames the way it already relays Recipe/Start to the setup responder.
  void onProfileListReq(const oven_ProfileListReq &m) override {
    if (mgmt_ != nullptr) {
      mgmt_->onProfileListReq(m);
    }
  }
  void onProfileGetReq(const oven_ProfileGetReq &m) override {
    if (mgmt_ != nullptr) {
      mgmt_->onProfileGetReq(m);
    }
  }
  void onProfilePut(const oven_ProfilePut &m) override {
    if (mgmt_ != nullptr) {
      mgmt_->onProfilePut(m);
    }
  }
  void onProfileDelete(const oven_ProfileDelete &m) override {
    if (mgmt_ != nullptr) {
      mgmt_->onProfileDelete(m);
    }
  }
  void onProfileDup(const oven_ProfileDup &m) override {
    if (mgmt_ != nullptr) {
      mgmt_->onProfileDup(m);
    }
  }
  void onProfileRename(const oven_ProfileRename &m) override {
    if (mgmt_ != nullptr) {
      mgmt_->onProfileRename(m);
    }
  }
  void onSettingsGetReq(const oven_SettingsGetReq &m) override {
    if (mgmt_ != nullptr) {
      mgmt_->onSettingsGetReq(m);
    }
  }
  void onSettingsPut(const oven_SettingsPut &m) override {
    if (mgmt_ != nullptr) {
      mgmt_->onSettingsPut(m);
    }
  }
  void onAbort() override {
    gate_.clearSession();
    if (executor_ != nullptr) {
      executor_->abort();
    }
  }

  // ISetupSink — an accepted Recipe is loaded for execution; an accepted Start
  // authorizes its session and begins the run.
  void onRecipeAccepted(const oven_Recipe &r) override {
    recipe_ = r;
    have_recipe_ = true;
    if (executor_ != nullptr) {
      // Reflow gates the hold-entry on measured workpiece temp; cure holds are dose
      // timers (§5). Derive that from recipe *content* (uv/motor => cure), never the
      // untrusted mode tag — the same rule oven_safety uses to pick the cap.
      const bool gated = oven_safety::deriveMode(r) == oven_Mode_MODE_REFLOW;
      executor_->load(r, gated);
    }
  }
  void onStartAccepted(const oven_Start &s) override {
    gate_.adoptSession(s.session);
    if (executor_ != nullptr && have_recipe_ && s.recipe_id == recipe_.id) {
      executor_->start();
    }
  }

private:
  protocol::AcceptAllValidator default_validator_;
  protocol::ISetupValidator &validator_;
  protocol::Handshake handshake_;
  protocol::SetupResponder responder_;
  SessionGate gate_;
  ProfileExecutor *executor_ = nullptr;
  ManagementResponder *mgmt_ = nullptr;
  oven_Recipe recipe_ = oven_Recipe_init_zero;
  bool have_recipe_ = false;
};
