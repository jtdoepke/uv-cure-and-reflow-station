// Esp32Clock — the IClock firmware adapter (design.md §11).
//
// Arduino millis(), nothing more. Firmware-only: it #includes <Arduino.h>, so it lives in
// src_control/ and never compiles for native_control, where every timeout-shaped decision above
// it — heartbeat freshness, the heater's proportioning window, the setup-path retry — is tested
// against FakeClock with time advanced by hand. That fakeability is the whole reason this is a
// port and not a direct millis() call (§11).
//
// src_cyd/esp32_clock.h is a deliberate twin: §11 puts adapters in each firmware env's src/,
// and the two trees never co-compile (build_src_filter). Don't merge them into lib/ — that
// would drag <Arduino.h> into the native test envs and break them.
#pragma once

#include <Arduino.h>

#include "IClock.h"

class Esp32Clock : public IClock {
public:
  uint32_t millis() override { return ::millis(); }
};
