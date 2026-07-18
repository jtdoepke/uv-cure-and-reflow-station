#include "request_client.h"

#include "link_params.h"

namespace protocol {

bool RequestClient::send(uint8_t type, const uint8_t *payload, size_t len, uint32_t seq) {
  if (state_ == State::Pending) {
    return false;
  }
  if (len > sizeof(buf_)) {
    return false;
  }
  type_ = type;
  len_ = len;
  for (size_t i = 0; i < len; ++i) {
    buf_[i] = payload[i];
  }
  pending_seq_ = seq;
  attempts_ = 0;
  state_ = State::Pending;
  last_send_ms_ = clock_.millis();
  link_.send(type_, buf_, len_);
  return true;
}

void RequestClient::resend() {
  last_send_ms_ = clock_.millis();
  ++attempts_;
  link_.send(type_, buf_, len_);
}

void RequestClient::service() {
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

void RequestClient::onReply(uint32_t seq) {
  if (state_ == State::Pending && seq == pending_seq_) {
    state_ = State::Done;
  }
}

void RequestClient::clear() {
  if (state_ != State::Pending) {
    state_ = State::Idle;
  }
}

} // namespace protocol
