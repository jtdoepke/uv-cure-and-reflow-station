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
  // The pin is injected (control_board.h::kContactorPin) rather than owned here: which GPIO drives
  // the coil is a fact about the board, and this class is a fact about Arduino.
  explicit Esp32Contactor(int pin) : pin_(pin) {}

  // Call first in setup(): OUTPUT + LOW (coil de-energized, mains isolated).
  void begin() {
    pinMode(pin_, OUTPUT);
    digitalWrite(pin_, LOW);
    begun_ = true;
  }

  void setClosed(bool closed) override {
    // See Esp32HeaterSwitch::set() — same contract, same reason. This adapter is the one that
    // actually hit it: SafetySupervisor's constructor calls setClosed(false) unconditionally
    // during static init, which the Arduino core logged once per boot as "IO 26 is not set as
    // GPIO". The heater escaped only because HeaterActuator::forceOff() de-dupes against its
    // already-off mirror and never reached the pin — the same latent bug, one output short of
    // firing it.
    if (!begun_) {
      return;
    }
    digitalWrite(pin_, closed ? HIGH : LOW);
  }

private:
  const int pin_;
  bool begun_ = false;
};
