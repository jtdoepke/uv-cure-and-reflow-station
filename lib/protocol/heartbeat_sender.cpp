#include "heartbeat_sender.h"

#include "codec.h"
#include "link_params.h"
#include "messages.h"

namespace protocol {

void HeartbeatSender::sendNow() {
  uint32_t now = clock_.millis();

  oven_Heartbeat hb = oven_Heartbeat_init_default;
  hb.session = session_;
  hb.seq = seq_++;
  hb.enable = enable_;
  hb.millis = now;

  uint8_t buf[oven_Heartbeat_size];
  size_t len = 0;
  if (encode(oven_Heartbeat_fields, &hb, buf, sizeof(buf), len)) {
    link_.send(kTfTypeHeartbeat, buf, len);
  }
  last_send_ms_ = now;
  sent_ = true;
}

void HeartbeatSender::service() {
  if (!sent_) {
    sendNow();
    return;
  }
  // Unsigned subtraction stays correct across millis() wraparound.
  if (static_cast<uint32_t>(clock_.millis() - last_send_ms_) >= kHeartbeatPeriodMs) {
    sendNow();
  }
}

} // namespace protocol
