// The controller's untrusted-input RX pipeline, wired cold on the stack — the scaffolding
// the "seam" harnesses (fuzz_frontdoor / fuzz_decode / fuzz_validator) share. This is the
// untrusted-input half of the suite; harnesses that fuzz internal correctness properties
// pull only fuzz_util.h, not this (fuzz/README.md).
#pragma once

#include "fuzz_util.h"

#include "codec.h"
#include "controller_link.h"
#include "frame_link.h"
#include "message_router.h"
#include "oven.pb.h"
#include "recipe_validator.h"

#include "helpers/fake_clock.h"
#include "helpers/pipe_transport.h"

namespace fuzz {

// The controller's whole RX pipeline, wired cold on the stack: bytes fed to feed()
// travel TinyFrame → MessageRouter → nanopb decode → ControllerLink → SetupResponder
// → RecipeValidator / SessionGate, exactly as on the real link. Build a fresh one
// per fuzzer input so no state leaks between cases. The handshake is deliberately
// NOT begun, so authorized() must stay false for any input (see fuzz_frontdoor).
struct ControllerHarness {
  LoopbackPipe pipe;
  FakeClock clock;
  protocol::MessageRouter router;
  protocol::FrameLink link{pipe.b(), TF_SLAVE, router}; // controller role
  RecipeValidator validator;
  ControllerLink ctrl{link, clock, &validator};

  ControllerHarness() { router.setObserver(ctrl); }

  // Inject attacker bytes onto the wire and pump the parser, like a UART read burst.
  void feed(const uint8_t *data, size_t len) {
    pipe.injectAToB(data, len);
    link.poll();
    link.tick(); // advance TinyFrame's resync-on-timeout clock
  }

  bool authorized() const { return ctrl.authorized(); }
};

} // namespace fuzz
