#include "request_responder.h"

#include "codec.h"

namespace protocol {

void RequestResponder::reply(uint32_t seq, uint8_t type, const pb_msgdesc_t *fields,
                             const void *msg) {
  uint8_t buf[sizeof(last_buf_)];
  size_t len = 0;
  if (!encode(fields, msg, buf, sizeof(buf), len)) {
    return; // reply doesn't fit the cache buffer — cannot happen for our reply set
  }
  last_seq_ = seq;
  last_type_ = type;
  last_len_ = len;
  for (size_t i = 0; i < len; ++i) {
    last_buf_[i] = buf[i];
  }
  have_last_ = true;
  link_.send(type, last_buf_, last_len_);
}

} // namespace protocol
