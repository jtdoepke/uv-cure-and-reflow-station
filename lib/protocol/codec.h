// Thin nanopb encode/decode wrappers for the shared protocol (design.md §9).
//
// Pure C++ over nanopb's C API — no Arduino, no LVGL — so every message
// round-trips in host tests (native_control). Callers pass the nanopb
// descriptor (e.g. oven_Dummy_fields) plus a struct instance; buffers are
// caller-owned, sized via the generated *_size constants.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include <pb_decode.h>
#include <pb_encode.h>

namespace protocol {

// Read a nanopb-decoded proto enum field as its raw wire integer. nanopb stores the
// decoded varint into the C enum field verbatim, so an untrusted peer can leave it
// holding a value outside the defined enumerators — and loading such an out-of-range
// enum *as its enum type* (e.g. `msg.mode == FOO`) is undefined behavior in C++
// (UBSAN's -fsanitize=enum flags it; a fuzzer found exactly this on recipe.mode).
// Compare the returned int32_t against the enumerator constants instead. Bool fields
// are safe — nanopb normalizes them to 0/1 — so this is only needed for enums.
template <typename E> inline int32_t wireEnum(const E &e) {
  static_assert(std::is_enum<E>::value, "wireEnum is for nanopb enum fields");
  static_assert(sizeof(E) <= sizeof(int32_t), "unexpected enum width");
  int32_t v = 0;
  std::memcpy(&v, &e, sizeof(E)); // read the storage without an enum-typed load
  return v;
}

// Normalize an untrusted enum field to a valid enumerator: returns it when its raw wire
// value is in [lo, hi] (pass the nanopb-generated _<Enum>_MIN / _MAX), else `fallback`.
// Use this — not wireEnum — whenever the value is *stored and later read as the enum type*:
// merely holding an out-of-range enum value is UB, so it must be sanitized on the way in.
// Assumes contiguous enumerators (true for this project's proto enums).
template <typename E> inline E wireEnumOr(const E &e, E lo, E hi, E fallback) {
  const int32_t v = wireEnum(e);
  if (v >= static_cast<int32_t>(lo) && v <= static_cast<int32_t>(hi)) {
    return static_cast<E>(v); // in range => a representable enumerator, no UB
  }
  return fallback;
}

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
