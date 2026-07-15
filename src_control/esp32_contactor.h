// Esp32Contactor — the IContactor firmware adapter (design.md §4, §11).
//
// A digitalWrite to the mains-isolation contactor's coil driver. Energize-to-close:
// setClosed(true) energizes the coil and lets mains through to the SSR. On the bench (backlog
// A8, §8 step 1) this GPIO drives an LED + series resistor standing in for the coil — the LED
// is the honest readout of ControllerLink::authorized(), since SafetySupervisor drives the
// contactor directly and only ever cuts. Firmware-only: it #includes <Arduino.h>, so it never
// compiles for native_control, where host tests use FakeContactor.
//
// The coil-drive GPIO is pulled DOWN in hardware, so a crashed, reset, brown-out, or
// bootloader-stuck MCU de-energizes the coil and the oven is mains-isolated with no firmware
// action. begin() is only the software half of that default; call it first in setup().
//
// Deliberately just a switch, like Esp32HeaterSwitch: the closed-only-while-a-run-is-actively-
// commanded policy is portable logic in SafetySupervisor (lib/control_logic), not this
// adapter's job.
#pragma once

#include <Arduino.h>

#include "IContactor.h"

class Esp32Contactor : public IContactor {
public:
  // Plain output: not a strapping pin (0/2/5/12/15), not input-only (34-39).
  static constexpr int kPin = 26;

  // Call first in setup(): OUTPUT + LOW (coil de-energized, mains isolated).
  void begin() {
    pinMode(kPin, OUTPUT);
    digitalWrite(kPin, LOW);
  }

  void setClosed(bool closed) override { digitalWrite(kPin, closed ? HIGH : LOW); }
};
