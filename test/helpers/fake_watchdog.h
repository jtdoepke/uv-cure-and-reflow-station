// Recording fake for IWatchdog — injected under the native_control env so the kick cadence
// and the reset-cause -> fault path are tested deterministically, no hardware. Header-only,
// shared via `#include "helpers/fake_watchdog.h"`.
#pragma once

#include "IWatchdog.h"

// Counts kicks so a test can assert the loop kicks exactly once per iteration (kicking more
// than once per loop would still pass a liveness check the loop had already failed), and lets
// a test stage the previous boot's reset cause.
struct FakeWatchdog : IWatchdog {
  int kicks = 0;
  ResetCause cause = ResetCause::PowerOn;
  void kick() override { kicks++; }
  ResetCause lastResetCause() const override { return cause; }
};
