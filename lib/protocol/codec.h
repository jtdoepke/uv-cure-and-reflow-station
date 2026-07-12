// Thin nanopb encode/decode wrappers for the shared protocol (design.md §9).
//
// Pure C++ over nanopb's C API — no Arduino, no LVGL — so every message
// round-trips in host tests (native_control). Callers pass the nanopb
// descriptor (e.g. oven_Dummy_fields) plus a struct instance; buffers are
// caller-owned, sized via the generated *_size constants.
#pragma once

#include <cstddef>
#include <cstdint>

#include <pb_decode.h>
#include <pb_encode.h>

namespace protocol {

// Serialize msg into buf. Returns false if the buffer is too small or the
// message violates its constraints; out_len is valid only on success.
inline bool encode(const pb_msgdesc_t *fields, const void *msg, uint8_t *buf, size_t buf_len,
                   size_t &out_len) {
  pb_ostream_t stream = pb_ostream_from_buffer(buf, buf_len);
  if (!pb_encode(&stream, fields, msg)) {
    return false;
  }
  out_len = stream.bytes_written;
  return true;
}

// Parse len bytes from buf into msg (which must be zero-initialized or a
// *_init_default). Returns false on malformed input.
inline bool decode(const pb_msgdesc_t *fields, void *msg, const uint8_t *buf, size_t len) {
  pb_istream_t stream = pb_istream_from_buffer(buf, len);
  return pb_decode(&stream, fields, msg);
}

} // namespace protocol
