// SetupResponder — the controller's half of the exactly-once setup path
// (design.md §9). Validates each Recipe/Start and answers Ack{seq} or
// Nak{seq,reason}, deduplicating retransmits so a dropped Ack (which makes the
// CYD resend) never applies a side effect twice.
//
// Validation is delegated to an ISetupValidator so A2 can ship a permissive
// default and A7 (recipe range / mode-content checks) can plug in real rules
// without touching this class. Accepted commands are surfaced through an
// optional ISetupSink — the ControllerLink facade uses it to hand an accepted
// Start's session to the SessionGate — keeping this protocol-layer class free of
// any control_logic dependency.
//
// Single outstanding command on the wire (the CYD's ReliableSender enforces it),
// so a lone last_processed_seq_ is enough to spot a duplicate: an in-order UART
// only ever redelivers the command currently being retried. Keep free of
// <Arduino.h>.
#pragma once

#include <cstdint>

#include "frame_link.h"
#include "messages.h"

namespace protocol {

// Upload-time validation hook. Return true to accept; on false set `reason`.
struct ISetupValidator {
  virtual ~ISetupValidator() = default;
  virtual bool validateRecipe(const oven_Recipe & /*recipe*/, oven_NakReason & /*reason*/) {
    return true;
  }
  virtual bool validateStart(const oven_Start & /*start*/, oven_NakReason & /*reason*/) {
    return true;
  }
};

// A2's default: accept everything. Real checks arrive with A7.
struct AcceptAllValidator : ISetupValidator {};

// Notified when a command is newly accepted (not on deduped retransmits).
struct ISetupSink {
  virtual ~ISetupSink() = default;
  virtual void onRecipeAccepted(const oven_Recipe & /*recipe*/) {}
  virtual void onStartAccepted(const oven_Start & /*start*/) {}
};

class SetupResponder {
public:
  // link and validator must outlive this responder.
  SetupResponder(FrameLink &link, ISetupValidator &validator)
      : link_(link), validator_(validator) {}

  // Optional: receive newly-accepted commands (e.g. to adopt a session).
  void setSink(ISetupSink &sink) { sink_ = &sink; }

  // Forget the last-processed seq. Call when the peer reboots (its Hello carries a
  // new boot_nonce): the CYD's ReliableSender re-seeds its seq on boot, so without
  // this a re-used seq would be mistaken for a retransmit and its side effect
  // (validate + accept) silently skipped (§9 re-sync).
  void reset() { have_last_ = false; }

  // Feed decoded setup messages (from MessageRouter).
  void onRecipe(const oven_Recipe &recipe);
  void onStart(const oven_Start &start);

private:
  bool isDuplicate(uint32_t seq);
  void replayCached();
  void accept(uint32_t seq);
  void reject(uint32_t seq, oven_NakReason reason);
  void sendAck(uint32_t seq);
  void sendNak(uint32_t seq, oven_NakReason reason);

  FrameLink &link_;
  ISetupValidator &validator_;
  ISetupSink *sink_ = nullptr;

  bool have_last_ = false;
  uint32_t last_seq_ = 0;
  bool last_was_ack_ = false;
  oven_NakReason last_reason_ = oven_NakReason_NAK_UNSPECIFIED;
};

} // namespace protocol
