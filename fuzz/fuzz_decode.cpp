// Seam 2: frame payload → MessageRouter → nanopb pb_decode, skipping TinyFrame's
// CRC gate so every input byte reaches the decoder (far higher decode density than
// the front door, where most mutations die at the checksum).
//
// Input format (matches the committed corpus, see seed_gen.cpp): byte 0 is the
// TinyFrame type id, the rest is the raw payload handed to protocol::decode.
#include "fuzz_util.h"

namespace {
// Default no-op observer: every dispatch target is exercised, nothing is retained
// past the call (the router's payload pointer is only valid during onFrame).
struct NullObserver : protocol::IMessageObserver {};
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size == 0) {
    return 0;
  }
  NullObserver obs;
  protocol::MessageRouter router(obs);
  router.onFrame(data[0], data + 1, size - 1);
  return 0;
}
