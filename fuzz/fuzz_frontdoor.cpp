// Seam 1 (highest value): raw attacker bytes → the controller's whole RX pipeline.
//
// Feeds the fuzzer input straight onto the wire and pumps the parser, so one input
// exercises TinyFrame's byte state machine (CRC-16, the 1024-byte RX buffer and its
// discard_data overflow path), nanopb pb_decode, the MessageRouter switch, and the
// SetupResponder/RecipeValidator/SessionGate backstop — in one shot, cold.
#include "fuzz_util.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  fuzz::ControllerHarness h;
  h.feed(data, size);

  // Sound safety invariant across the whole stack: a run may only ever be authorized
  // for a non-zero session (session 0 is the IDLE telemetry sentinel and must never
  // be adoptable — the f86f48f backstop). This holds for legitimate input too, so
  // splicing the valid Hello/Recipe/Start/Heartbeat seeds into one authorizing input
  // is fine — only a regression that let session 0 through the validator, the setup
  // dedup, and the gate would trip it. (Memory-safety / UB coverage of TinyFrame +
  // nanopb + the router is the sanitizers' job and needs no assertion.)
  if (h.authorized()) {
    FUZZ_ASSERT(h.ctrl.gate().activeSession() != 0);
  }
  return 0;
}
