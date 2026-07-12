#include "tf_glue.h"

namespace protocol {

void bindTransport(TinyFrame &tf, ISerialTransport &transport) {
  tf.userdata = &transport;
}

} // namespace protocol

// TinyFrame's required output hook (declared in TinyFrame.h, called from
// TinyFrame.c) — C linkage to match the C-compiled caller.
extern "C" void TF_WriteImpl(TinyFrame *tf, const uint8_t *buff, uint32_t len) {
  auto *transport = static_cast<ISerialTransport *>(tf->userdata);
  if (transport != nullptr) {
    transport->write(buff, len);
  }
}
