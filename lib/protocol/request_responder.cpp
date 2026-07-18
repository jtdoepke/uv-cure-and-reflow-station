#include "request_responder.h"

#include "codec.h"

namespace protocol {

void RequestResponder::reply(uint32_t seq, uint8_t type, const pb_msgdesc_t *fields,
                             const void *msg) {
  // Encode straight into the dedup cache — no intermediate stack buffer (this runs several frames
  // deep behind the FrameLink poll, where a spare ~1.5 KB frame is worth avoiding). On an encode
  // failure the previous cached reply is left intact.
  size_t len = 0;
  if (!encode(fields, msg, last_buf_, sizeof(last_buf_), len)) {
    return; // reply doesn't fit the cache buffer — cannot happen for our reply set
  }
  last_seq_ = seq;
  last_type_ = type;
  last_len_ = len;
  have_last_ = true;
  link_.send(type, last_buf_, last_len_);
}

} // namespace protocol
