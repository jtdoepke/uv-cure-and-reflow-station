// Recording fake for IContactor — injected into SafetySupervisor under the
// native_control env so the contactor policy is tested deterministically, no
// hardware. Header-only, shared via `#include "helpers/fake_contactor.h"`.
#pragma once

#include "IContactor.h"

// Records the current coil state plus counts, so tests can assert the contactor is
// open/closed and check we don't emit redundant coil-drive writes.
struct FakeContactor : IContactor {
  bool closed = false;
  int setCalls = 0;    // total setClosed() invocations
  int transitions = 0; // setClosed() calls that actually changed state
  void setClosed(bool v) override {
    setCalls++;
    if (v != closed) {
      closed = v;
      transitions++;
    }
  }
};
