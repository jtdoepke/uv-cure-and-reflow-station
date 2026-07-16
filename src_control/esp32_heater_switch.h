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
  // The pin is injected (control_board.h::kHeaterPin) rather than owned here: which GPIO drives
  // the SSR is a fact about the board, and this class is a fact about Arduino.
  explicit Esp32HeaterSwitch(int pin) : pin_(pin) {}

  // Call first in setup(): OUTPUT + LOW before any logic can command heat.
  void begin() {
    pinMode(pin_, OUTPUT);
    digitalWrite(pin_, LOW);
    begun_ = true;
  }

  void set(bool on) override {
    // Before begin(), the pin is high-Z and the hardware pull-down — not us — is holding the SSR
    // off. A digitalWrite here cannot change that; the Arduino core would only log
    // "IO N is not set as GPIO". Drop it rather than pretend, and keep begin()'s LOW as the one
    // definition of the boot state, so no pre-init command can ever be replayed into heat.
    // This is not hypothetical: SafetySupervisor's constructor commands both outputs safe, and it
    // runs during static init, before setup() exists to have called begin().
    if (!begun_) {
      return;
    }
    digitalWrite(pin_, on ? HIGH : LOW);
  }

private:
  const int pin_;
  bool begun_ = false;
};
