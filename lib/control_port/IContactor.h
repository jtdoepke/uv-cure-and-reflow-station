// IContactor — the mains-isolation contactor coil (design.md §4).
//
// Energize-to-close: setClosed(true) energizes the coil and lets mains through to
// the SSR; setClosed(false) drops it and removes mains by construction. The pair
// "SSR modulates, contactor isolates" — the SSR (IHeaterSwitch) does the fast
// zero-cross power control, the contactor is the coarse run-active isolation.
//
// The production adapter's coil-drive GPIO is pulled *down* in hardware, so a
// crashed, reset, brown-out, or bootloader-stuck MCU de-energizes the coil and the
// oven is mains-isolated without any firmware action (fail-safe open).
//
// Deliberately just a switch, like IHeaterSwitch: the closed-only-while-a-run-is-
// active policy is portable logic in lib/control_logic (SafetySupervisor, §4), not
// the port's job, so it stays host-testable against a recording fake.
//
// Keep this header free of <Arduino.h> so it stays native-compilable.
#pragma once

struct IContactor {
  virtual ~IContactor() = default;
  virtual void setClosed(bool closed) = 0;
};
