// Esp32HeaterSwitch — the IHeaterSwitch firmware adapter (design.md §11, §4).
//
// A digitalWrite to the zero-cross SSR's gate. On the bench (backlog A8, §8 step 1) this GPIO
// drives an LED + series resistor standing in for the SSR — same firmware, same adapter, just a
// dummy load; D3 re-runs the fail-safe proof against the real chain. Firmware-only: it
// #includes <Arduino.h>, so — like the rest of src_control/ — it never compiles for
// native_control, where host tests drive HeaterActuator through FakeHeaterSwitch instead.
//
// The gate is pulled DOWN in hardware, so a crashed, reset, brown-out, or bootloader-stuck MCU
// is OFF with no firmware action at all. begin() is only the software half of that default;
// call it first in setup(), before anything can command heat.
//
// Deliberately just a switch: time-proportioning (duty -> on/off windows) is portable logic in
// HeaterActuator (lib/control_logic), not this adapter's job, so it stays host-testable.
#pragma once

#include <Arduino.h>

#include "IHeaterSwitch.h"

class Esp32HeaterSwitch : public IHeaterSwitch {
public:
  // Plain output: not a strapping pin (0/2/5/12/15), not input-only (34-39).
  static constexpr int kPin = 25;

  // Call first in setup(): OUTPUT + LOW before any logic can command heat.
  void begin() {
    pinMode(kPin, OUTPUT);
    digitalWrite(kPin, LOW);
  }

  void set(bool on) override { digitalWrite(kPin, on ? HIGH : LOW); }
};
