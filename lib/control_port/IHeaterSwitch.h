// IHeaterSwitch — bare on/off gate for the heater SSR (design.md §11).
//
// Deliberately just a switch: time-proportioning (duty -> on/off windows) is
// portable logic in lib/control_logic, NOT the port's job, so it stays
// host-testable against a recording fake. The production adapter drives the
// zero-cross SSR gate GPIO, which is pulled down in hardware (fail-safe OFF).
//
// Keep this header free of <Arduino.h> so it stays native-compilable.
#pragma once

struct IHeaterSwitch {
  virtual ~IHeaterSwitch() = default;
  virtual void set(bool on) = 0;
};
