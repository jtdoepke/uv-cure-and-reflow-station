// IWatchdog — the controller's hardware watchdog (design.md §9, §11).
//
// Two capabilities, one port, because they are two halves of one story: kick() asserts that
// the main loop is still running, and lastResetCause() lets the *next* boot discover that the
// previous one was killed by the watchdog — so it can report Fault{WATCHDOG} (§9) instead of
// coming back silently as though nothing happened.
//
// The watchdog is the backstop for a *hung* loop, not for a bad decision. SafetySupervisor
// already cuts the outputs on any loss of authorization, but if the loop stops running at all
// then tick() stops with it and the pins would hold their last state — so the watchdog resets
// the MCU and the hardware pull-downs take heater and contactor to safe with no firmware
// involved (§11). That is also why kick() must only ever be called from the main loop itself:
// kicking from a timer or a second task would prove the timer is alive, which is not the
// question being asked.
//
// Keep this header free of <Arduino.h> so it stays native-compilable, and free of the
// generated oven.pb.h — mapping ResetCause onto oven_FaultCode is A4b's job, not the port's.
#pragma once

// Why the MCU last came up. Deliberately coarse: the controller only needs to tell "the
// watchdog or a crash killed us, say so" from "this is an ordinary boot".
enum class ResetCause {
  PowerOn,  // cold boot or the EN pin — an ordinary start, nothing to report
  Watchdog, // the loop hung and the watchdog fired -> Fault{WATCHDOG} (A4b)
  Panic,    // firmware crash: exception, abort, or a failed assert
  Software, // a deliberate restart we asked for
  Brownout, // the supply dipped below the detector's threshold
  Other,    // anything the adapter can't classify
};

struct IWatchdog {
  virtual ~IWatchdog() = default;

  // Assert that the main loop is alive. Call once per loop iteration, from the loop.
  virtual void kick() = 0;

  // Why the MCU last reset. Constant for the lifetime of this boot.
  virtual ResetCause lastResetCause() const = 0;
};
