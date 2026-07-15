#include "telemetry_sender.h"

#include "codec.h"
#include "link_params.h"

namespace protocol {

void TelemetrySender::sendNow() {
  uint32_t now = clock_.millis();

  state_.session = session_;
  state_.seq = seq_++;
  state_.ctrl_millis = now;

  uint8_t buf[oven_Telemetry_size];
  size_t len = 0;
  if (encode(oven_Telemetry_fields, &state_, buf, sizeof(buf), len)) {
    link_.send(kTfTypeTelemetry, buf, len);
  }
  last_send_ms_ = now;
  sent_ = true;
}

void TelemetrySender::service() {
  if (!sent_) {
    sendNow();
    return;
  }
  // Unsigned subtraction stays correct across millis() wraparound.
  if (static_cast<uint32_t>(clock_.millis() - last_send_ms_) >= kTelemetryPeriodMs) {
    sendNow();
  }
}

} // namespace protocol
