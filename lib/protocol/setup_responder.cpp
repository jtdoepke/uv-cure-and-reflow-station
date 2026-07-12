#include "setup_responder.h"

#include "codec.h"

namespace protocol {

bool SetupResponder::isDuplicate(uint32_t seq) {
  return have_last_ && seq == last_seq_;
}

void SetupResponder::replayCached() {
  if (last_was_ack_) {
    sendAck(last_seq_);
  } else {
    sendNak(last_seq_, last_reason_);
  }
}

void SetupResponder::accept(uint32_t seq) {
  have_last_ = true;
  last_seq_ = seq;
  last_was_ack_ = true;
  sendAck(seq);
}

void SetupResponder::reject(uint32_t seq, oven_NakReason reason) {
  have_last_ = true;
  last_seq_ = seq;
  last_was_ack_ = false;
  last_reason_ = reason;
  sendNak(seq, reason);
}

void SetupResponder::onRecipe(const oven_Recipe &recipe) {
  if (isDuplicate(recipe.seq)) {
    replayCached(); // resend the same verdict; skip the side effect
    return;
  }
  oven_NakReason reason = oven_NakReason_NAK_UNSPECIFIED;
  if (!validator_.validateRecipe(recipe, reason)) {
    reject(recipe.seq, reason);
    return;
  }
  accept(recipe.seq);
  if (sink_ != nullptr) {
    sink_->onRecipeAccepted(recipe);
  }
}

void SetupResponder::onStart(const oven_Start &start) {
  if (isDuplicate(start.seq)) {
    replayCached();
    return;
  }
  oven_NakReason reason = oven_NakReason_NAK_UNSPECIFIED;
  if (!validator_.validateStart(start, reason)) {
    reject(start.seq, reason);
    return;
  }
  accept(start.seq);
  if (sink_ != nullptr) {
    sink_->onStartAccepted(start);
  }
}

void SetupResponder::sendAck(uint32_t seq) {
  oven_Ack ack = oven_Ack_init_default;
  ack.seq = seq;
  uint8_t buf[oven_Ack_size];
  size_t len = 0;
  if (encode(oven_Ack_fields, &ack, buf, sizeof(buf), len)) {
    link_.send(kTfTypeAck, buf, len);
  }
}

void SetupResponder::sendNak(uint32_t seq, oven_NakReason reason) {
  oven_Nak nak = oven_Nak_init_default;
  nak.seq = seq;
  nak.reason = reason;
  uint8_t buf[oven_Nak_size];
  size_t len = 0;
  if (encode(oven_Nak_fields, &nak, buf, sizeof(buf), len)) {
    link_.send(kTfTypeNak, buf, len);
  }
}

} // namespace protocol
