#include "handshake.h"

#include "codec.h"
#include "link_params.h"
#include "schema.h"

namespace protocol {

void Handshake::sendHello() {
  oven_Hello hello = oven_Hello_init_default;
  hello.proto_ver = kProtoVer;
  hello.schema_hash = kSchemaHash;
  hello.fw_ver = fw_ver_;
  hello.caps = caps_;

  uint8_t buf[oven_Hello_size];
  size_t len = 0;
  if (encode(oven_Hello_fields, &hello, buf, sizeof(buf), len)) {
    link_.send(kTfTypeHello, buf, len);
  }
  last_send_ms_ = clock_.millis();
  sent_ = true;
}

void Handshake::service() {
  if (saw_peer_) {
    return; // peer answered; stop announcing ourselves
  }
  if (!sent_) {
    sendHello();
    return;
  }
  // Unsigned subtraction stays correct across millis() wraparound.
  if (static_cast<uint32_t>(clock_.millis() - last_send_ms_) >= kHelloRetryMs) {
    sendHello();
  }
}

void Handshake::onPeerHello(const oven_Hello &hello) {
  peer_ = hello;
  saw_peer_ = true;
  matched_ = (hello.proto_ver == kProtoVer && hello.schema_hash == kSchemaHash);
}

} // namespace protocol
