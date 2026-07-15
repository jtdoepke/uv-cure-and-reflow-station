// Esp32Clock — the IClock firmware adapter (design.md §11).
//
// Arduino millis(), nothing more. Firmware-only: it #includes <Arduino.h>, so it lives in
// src_cyd/ and never compiles for the native test targets.
//
// This exists purely for the link stack: CydLink's Handshake / HeartbeatSender / ReliableSender
// read the clock internally (they own cadences, so they must), unlike the CYD's other logic —
// AutoBrightness, SleepController — which take `now` as a tick argument. Host tests inject
// FakeClock here and advance time by hand, which is how the 200 ms heartbeat period and the
// setup-path retry timeouts are tested with no board.
//
// src_control/esp32_clock.h is a deliberate twin: §11 puts adapters in each firmware env's
// src/, and the two trees never co-compile (build_src_filter). Don't merge them into lib/ —
// that would drag <Arduino.h> into the native test envs and break them.
#pragma once

#include <Arduino.h>

#include "IClock.h"

class Esp32Clock : public IClock {
public:
  uint32_t millis() override { return ::millis(); }
};
