#include "handshake.h"

#include "codec.h"
#include "link_params.h"
#include "schema.h"

namespace protocol {

void Handshake::begin(uint32_t boot_nonce) {
  boot_nonce_ = boot_nonce;
  sendHello();
}

void Handshake::sendHello() {
  oven_Hello hello = oven_Hello_init_default;
  hello.proto_ver = kProtoVer;
  hello.schema_hash = kSchemaHash;
  hello.fw_ver = fw_ver_;
  hello.caps = caps_;
  hello.boot_nonce = boot_nonce_;

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

bool Handshake::onPeerHello(const oven_Hello &hello) {
  // Read the remembered nonce before peer_ is overwritten. "New" covers both a peer we
  // have never heard and one whose nonce changed, i.e. a peer that rebooted.
  const bool peer_is_new = !saw_peer_ || hello.boot_nonce != peer_.boot_nonce;

  peer_ = hello;
  saw_peer_ = true;
  matched_ = (hello.proto_ver == kProtoVer && hello.schema_hash == kSchemaHash);

  // Hearing a peer is not the same as being heard, and service() goes quiet the moment
  // saw_peer_ flips — so whoever hears first would otherwise stop announcing without
  // ever confirming, leaving the other side to announce into a void forever. That is
  // not a corner case: the CYD spends ~3 s in its boot self-test before it starts
  // listening, so it loses that race every time (the bench showed exactly this —
  // controller matched=1 while the CYD sat at sawPeer=0 with peer=00000000).
  //
  // So answer. A new or changed nonce means the peer certainly does not know us yet,
  // and gets an immediate reply. Otherwise still answer, but at most once per
  // kHelloRetryMs: that covers a reply that got lost (the peer keeps announcing, we
  // keep answering until one lands) while stopping two live boards from answering each
  // other's answers forever — the rate limit is what makes this terminate, since our
  // reply is never "new" to a peer that already knows us and cannot bounce back.
  const bool due =
      !sent_ || static_cast<uint32_t>(clock_.millis() - last_send_ms_) >= kHelloRetryMs;
  if (peer_is_new || due) {
    sendHello();
  }
  return peer_is_new;
}

} // namespace protocol
