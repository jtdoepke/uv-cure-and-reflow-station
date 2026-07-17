#include "fault_sender.h"

#include "codec.h"
#include "link_params.h"
#include "messages.h"

namespace protocol {

void FaultSender::sendNow() {
  oven_Fault m = oven_Fault_init_default;
  m.session = session_;
  m.code = code_;

  uint8_t buf[oven_Fault_size];
  size_t len = 0;
  if (encode(oven_Fault_fields, &m, buf, sizeof(buf), len)) {
    link_.send(kTfTypeFault, buf, len);
  }
  last_send_ms_ = clock_.millis();
}

void FaultSender::set(oven_FaultCode code) {
  if (code == code_) {
    return;
  }
  code_ = code;
  if (code_ != oven_FaultCode_FAULT_NONE) {
    sendNow(); // fire-on-change: announce the new fault at once
  }
}

void FaultSender::service() {
  if (code_ == oven_FaultCode_FAULT_NONE) {
    return; // nothing active to re-announce
  }
  // Unsigned subtraction stays correct across millis() wraparound.
  if (static_cast<uint32_t>(clock_.millis() - last_send_ms_) >= kFaultResendMs) {
    sendNow();
  }
}

} // namespace protocol
