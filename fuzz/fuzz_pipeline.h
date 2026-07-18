// The controller's untrusted-input RX pipeline, wired cold on the stack — the scaffolding
// the "seam" harnesses (fuzz_frontdoor / fuzz_decode / fuzz_validator) share. This is the
// untrusted-input half of the suite; harnesses that fuzz internal correctness properties
// pull only fuzz_util.h, not this (fuzz/README.md).
#pragma once

#include "fuzz_util.h"

#include <cstring>

#include "codec.h"
#include "controller_link.h"
#include "device_settings.h"
#include "frame_link.h"
#include "management_responder.h"
#include "message_router.h"
#include "oven.pb.h"
#include "profile_library.h"
#include "recipe_validator.h"

#include "helpers/fake_clock.h"
#include "helpers/fake_profile_storage.h"
#include "helpers/fake_settings_storage.h"
#include "helpers/pipe_transport.h"

namespace fuzz {

// A small valid profile to pre-seed a store, so a fuzzed Get/Delete/Dup/Rename exercises the
// found path (not just not-found) and a Put exercises overwrite.
inline oven_Profile seedProfile(oven_Mode mode, const char *name) {
  oven_Profile p = oven_Profile_init_zero;
  p.mode = mode;
  std::strncpy(p.name, name, sizeof(p.name) - 1);
  p.phases_count = 1;
  p.phases[0].target_c = mode == oven_Mode_MODE_REFLOW ? 245.0F : 80.0F;
  return p;
}

// The controller's whole RX pipeline, wired cold on the stack: bytes fed to feed()
// travel TinyFrame → MessageRouter → nanopb decode → ControllerLink → SetupResponder
// → RecipeValidator / SessionGate, AND the profile-management path → ManagementResponder →
// control::ProfileStore → nanopb decode, exactly as on the real link. Build a fresh one per
// fuzzer input so no state leaks between cases. The handshake is deliberately NOT begun, so
// authorized() must stay false for any input (see fuzz_frontdoor). The profile stores are the
// untrusted-blob surface the §2 "CYD is a UI remote" split (2026-07-17) put on the safety MCU —
// the sanitizers catch any memory/UB bug in decoding a fuzzed ProfilePut/Get/List/etc.
struct ControllerHarness {
  LoopbackPipe pipe;
  FakeClock clock;
  protocol::MessageRouter router;
  protocol::FrameLink link{pipe.b(), TF_SLAVE, router}; // controller role
  RecipeValidator validator;
  ControllerLink ctrl{link, clock, &validator};
  FakeProfileStorage cure_fs;
  FakeProfileStorage reflow_fs;
  control::ProfileStore cure_store{cure_fs, oven_Mode_MODE_CURE};
  control::ProfileStore reflow_store{reflow_fs, oven_Mode_MODE_REFLOW};
  FakeSettingsStorage settings_fs;
  control::SettingsStore settings_store{settings_fs};
  ManagementResponder mgmt{link, cure_store, reflow_store};

  ControllerHarness() {
    router.setObserver(ctrl);
    mgmt.setSettingsStore(settings_store);
    ctrl.setManagementResponder(mgmt);
    cure_store.save(seedProfile(oven_Mode_MODE_CURE, "Resin-A"));
    reflow_store.save(seedProfile(oven_Mode_MODE_REFLOW, "LF-245"));
  }

  // Inject attacker bytes onto the wire and pump the parser, like a UART read burst.
  void feed(const uint8_t *data, size_t len) {
    pipe.injectAToB(data, len);
    link.poll();
    link.tick(); // advance TinyFrame's resync-on-timeout clock
  }

  bool authorized() const { return ctrl.authorized(); }
};

} // namespace fuzz
