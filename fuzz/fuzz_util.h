// Shared scaffolding for the libFuzzer harnesses in this directory (fuzz/README.md).
//
// A harness is meant to be a dozen lines: it includes this header, carves the raw
// input with FuzzedDataProvider, and drives one seam of the untrusted-input path.
// The reusable parts — the cold controller pipeline and the invariant macro — live
// here so new harnesses reuse instead of copy.
//
// Host-only, clang-only: this header pulls <fuzzer/FuzzedDataProvider.h> from the
// (mise-pinned) clang toolchain, so only the fuzz_* harness envs include it. The
// seed generator (seed_gen.cpp), which builds under plain GCC, must NOT include it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <fuzzer/FuzzedDataProvider.h> // from the clang resource dir (-fsanitize=fuzzer builds)

#include "codec.h"
#include "controller_link.h"
#include "frame_link.h"
#include "message_router.h"
#include "oven.pb.h"
#include "recipe_validator.h"

#include "helpers/fake_clock.h"
#include "helpers/pipe_transport.h"

// Always-abort invariant check. Deliberately not <cassert>'s assert: a release-mode
// -DNDEBUG build would compile that to nothing, silently disarming every semantic
// invariant. A fuzzer with disarmed postconditions finds only crashes.
#define FUZZ_ASSERT(cond)                                                                          \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::fprintf(stderr, "FUZZ_ASSERT failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);         \
      std::abort();                                                                                \
    }                                                                                              \
  } while (0)

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
