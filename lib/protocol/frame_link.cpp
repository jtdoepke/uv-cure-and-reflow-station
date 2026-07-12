#include "frame_link.h"

namespace protocol {

FrameLink::FrameLink(ISerialTransport &transport, TF_Peer peer, IFrameHandler &handler)
    : transport_(transport), handler_(handler) {
  TF_InitStatic(&tf_, peer);
  tf_.userdata = this; // how TF_WriteImpl and onGeneric find their way back
  TF_AddGenericListener(&tf_, &FrameLink::onGeneric);
}

bool FrameLink::send(uint8_t type, const uint8_t *payload, size_t len) {
  TF_Msg msg;
  TF_ClearMsg(&msg);
  msg.type = type;
  msg.data = payload;
  msg.len = static_cast<TF_LEN>(len);
  return TF_Send(&tf_, &msg);
}

void FrameLink::poll() {
  uint8_t buf[64];
  for (;;) {
    size_t n = transport_.read(buf, sizeof(buf));
    if (n == 0) {
      return;
    }
    TF_Accept(&tf_, buf, static_cast<uint32_t>(n));
  }
}

void FrameLink::tick() {
  TF_Tick(&tf_);
}

TF_Result FrameLink::onGeneric(TinyFrame *tf, TF_Msg *msg) {
  auto *self = static_cast<FrameLink *>(tf->userdata);
  if (self != nullptr) {
    self->handler_.onFrame(static_cast<uint8_t>(msg->type), msg->data,
                           static_cast<size_t>(msg->len));
  }
  return TF_STAY; // keep the listener registered for the next frame
}

} // namespace protocol

// TinyFrame's required output hook (declared in TinyFrame.h, called from
// TinyFrame.c). C linkage to match the C-compiled caller; this is the ONE
// definition in the whole program. It routes emitted bytes to the transport of
// whichever FrameLink owns this TinyFrame instance (stored in tf->userdata),
// dropping bytes if none is bound.
extern "C" void TF_WriteImpl(TinyFrame *tf, const uint8_t *buff, uint32_t len) {
  auto *link = static_cast<protocol::FrameLink *>(tf->userdata);
  if (link != nullptr) {
    link->transport().write(buff, len);
  }
}
