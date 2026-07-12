// Recording fake for IHeaterSwitch — injected into HeaterActuator under the
// native_control env so time-proportioning is tested deterministically against a
// FakeClock, no hardware. Header-only, shared via `#include "helpers/fake_heater_switch.h"`.
#pragma once

#include "IHeaterSwitch.h"

// Records the current line state plus counts, so tests can assert on-time (by sampling
// `on` while advancing the clock) and check we don't emit redundant GPIO writes.
struct FakeHeaterSwitch : IHeaterSwitch {
  bool on = false;
  int setCalls = 0;    // total set() invocations
  int transitions = 0; // set() calls that actually changed state
  void set(bool v) override {
    setCalls++;
    if (v != on) {
      on = v;
      transitions++;
    }
  }
};
