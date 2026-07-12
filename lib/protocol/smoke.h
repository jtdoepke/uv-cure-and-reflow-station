// M0.2 smoke hook: proves the nanopb codegen + runtime link in whatever binary
// calls it (both firmware mains log the result at boot). Replaced by the real
// Hello/schema-hash constants in M0.3.
#pragma once

#include <cstddef>

#include "codec.h"
#include "oven.pb.h"

namespace protocol {

// Encoded size of a canary Dummy message; 0 signals an encode failure.
inline size_t dummyEncodedSize() {
  oven_Dummy msg = oven_Dummy_init_default;
  msg.value = 42;
  uint8_t buf[oven_Dummy_size];
  size_t len = 0;
  if (!encode(oven_Dummy_fields, &msg, buf, sizeof(buf), len)) {
    return 0;
  }
  return len;
}

} // namespace protocol
