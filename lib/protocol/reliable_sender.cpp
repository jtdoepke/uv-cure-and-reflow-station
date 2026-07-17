#include "reliable_sender.h"

#include "codec.h"
#include "link_params.h"

namespace protocol {

bool ReliableSender::sendRecipe(const oven_Recipe &recipe) {
  if (state_ == State::Pending) {
    return false;
  }
  oven_Recipe msg = recipe;
  msg.seq = ++seq_;

  uint8_t buf[oven_Recipe_size];
  size_t len = 0;
  if (!encode(oven_Recipe_fields, &msg, buf, sizeof(buf), len)) {
    return false;
  }
  return begin(kTfTypeRecipe, buf, len);
}

bool ReliableSender::sendStart(const oven_Start &start) {
  if (state_ == State::Pending) {
    return false;
  }
  oven_Start msg = start;
  msg.seq = ++seq_;

  uint8_t buf[oven_Start_size];
  size_t len = 0;
  if (!encode(oven_Start_fields, &msg, buf, sizeof(buf), len)) {
    return false;
  }
  return begin(kTfTypeStart, buf, len);
}

bool ReliableSender::begin(uint8_t type, const uint8_t *payload, size_t len) {
  // Cache for retransmit, then emit the initial copy.
  type_ = type;
  len_ = len;
  for (size_t i = 0; i < len; ++i) {
    buf_[i] = payload[i];
  }
  pending_seq_ = seq_;
  attempts_ = 0;
  state_ = State::Pending;
  last_send_ms_ = clock_.millis();
  link_.send(type_, buf_, len_);
  return true;
}

void ReliableSender::resend() {
  last_send_ms_ = clock_.millis();
  ++attempts_;
  link_.send(type_, buf_, len_);
}

void ReliableSender::service() {
  if (state_ != State::Pending) {
    return;
  }
  // Unsigned subtraction stays correct across millis() wraparound.
  if (static_cast<uint32_t>(clock_.millis() - last_send_ms_) < kSetupAckTimeoutMs) {
    return;
  }
  if (attempts_ >= kSetupMaxRetries) {
    state_ = State::Failed;
    return;
  }
  resend();
}

void ReliableSender::onAck(const oven_Ack &ack) {
  if (state_ == State::Pending && ack.seq == pending_seq_) {
    state_ = State::Acked;
  }
}

void ReliableSender::onNak(const oven_Nak &nak) {
  if (state_ == State::Pending && nak.seq == pending_seq_) {
    state_ = State::Nakd;
    // nak.reason is untrusted (nanopb stores proto enums raw): a peer could send a value
    // outside the enumerators, and we STORE it and later hand it out via lastNakReason(), so
    // it must be a valid enumerator or every read is UB. Unknown reasons fold to UNSPECIFIED
    // (the generic bucket the UI already shows for codes it doesn't recognize).
    last_nak_reason_ = wireEnumOr(nak.reason, _oven_NakReason_MIN, _oven_NakReason_MAX,
                                  oven_NakReason_NAK_UNSPECIFIED);
  }
}

} // namespace protocol
