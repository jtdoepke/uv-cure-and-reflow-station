// Esp32DoorSensor — the IDoorSensor firmware adapter (design.md §4/§6/§15, backlog D3).
//
// Reads the donor's DS3 dry contact (COM+NO, actuated by the bottom latch) on a GPIO with the
// internal pull-up. DS3 closes when the door SHUTS, so:
//
//     pin LOW  = contact closed = door shut
//     pin HIGH = contact open   = door open  ← also what a cut/unplugged sense wire reads
//
// That polarity is the whole point of the wiring choice: a broken sense line reads DOOR OPEN, so it
// blocks a run (§19's Start gate) rather than silently claiming the door is shut. See
// lib/control_port/IDoorSensor.h for why DS3 and never DS1/DS2.
//
// Debounced by time, not by sampling: isOpen() is called from the control loop at loop rate, so a
// contact that is still bouncing simply keeps reporting the last settled level until it has held a
// new one for kDoorDebounceMs. That keeps a bouncing contact from ending a run on a spurious edge —
// while still ending it well within human reaction time on a real open.
//
// Firmware-only: it #includes <Arduino.h>, so it never compiles for native_control, where host
// tests use FakeDoorSensor.
//
// NOT a safety device. The hardware interlock (DS1, in the line conductor) removes heater power
// with no firmware involvement; this only tells the firmware what happened so the UI can explain
// it. Nothing here may ever become the thing that cuts power (§4 L0).
#pragma once

#include <Arduino.h>

#include "IDoorSensor.h"

class Esp32DoorSensor : public IDoorSensor {
public:
  // Pin + debounce window injected from control_board.h — which GPIO and how long are facts about
  // the board; this class is a fact about Arduino.
  Esp32DoorSensor(int pin, uint32_t debounceMs) : pin_(pin), debounce_ms_(debounceMs) {}

  // Call in setup(). Seeds the debounce state from the pin's ACTUAL level rather than assuming
  // shut: booting with the door open must report open immediately, not after one debounce window
  // of claiming the oven is closed.
  void begin() {
    pinMode(pin_, INPUT_PULLUP);
    stable_ = raw();
    candidate_ = stable_;
    changed_at_ = millis();
    begun_ = true;
  }

  bool isOpen() const override {
    if (!begun_) {
      return true; // fail safe: never claim "shut" from a pin we have not configured
    }
    const bool now = raw();
    const uint32_t t = millis();
    if (now != candidate_) {
      candidate_ = now;
      changed_at_ = t;
    } else if (now != stable_ && static_cast<uint32_t>(t - changed_at_) >= debounce_ms_) {
      stable_ = now;
    }
    return stable_;
  }

private:
  // HIGH = contact open = door open (see the polarity note above).
  bool raw() const { return digitalRead(pin_) == HIGH; }

  int pin_;
  uint32_t debounce_ms_;
  // Mutable because isOpen() is const by port contract (it is a *query*) while debouncing is
  // inherently stateful. The alternative — a non-const service() the caller must remember to pump —
  // buys nothing and adds a way to get it wrong.
  mutable bool stable_ = true; // fail-safe initial value, replaced by begin()
  mutable bool candidate_ = true;
  mutable uint32_t changed_at_ = 0;
  bool begun_ = false;
};
