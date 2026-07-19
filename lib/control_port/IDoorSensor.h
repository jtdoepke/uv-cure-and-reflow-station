// IDoorSensor — is the oven door open? (design.md §4/§6/§15, backlog D3.)
//
// A *sense* channel only. It never gates power: the hardware interlock does that, in the line
// conductor, with no firmware involvement (§4 L0). This port exists because an interlock that
// merely kills power leaves the firmware blind — the CYD cannot say "close the door", gate Start
// (§19), or end a run cleanly (§15) if it has no idea why the heater stopped responding.
//
// WHICH SWITCH (from the donor teardown — this matters and is easy to get wrong):
//
//   DS1  primary interlock, top latch  — carries the 12.5 A heater feed. NEVER wire logic to it.
//   DS2  monitor switch,   bottom latch — a SACRIFICIAL short that blows the 20 A fuse if DS1
//                                         welds. Not a sensor; hanging anything off it both taps
//                                         mains and risks defeating its one job.
//   DS3  door sense,       bottom latch — a DRY contact, no load current, already routed to the
//                                         donor's control board. THIS one. ✅
//
// The teardown is explicit that DS1's spare NC contact is "tempting as a door-open signal — do not
// use it. It is mains-referenced; tapping it runs mains potential into logic wiring," and requires
// two independent paths: the interlock in the line conductor (must not depend on the controller)
// and a separate low-voltage contact for firmware state (must not touch mains).
//
// POLARITY FAILS SAFE TO OPEN. DS3 is wired COM+NO — closed when the door is shut — so the adapter
// reads it with a pull-up: shut pulls the pin low, and a severed or unplugged sense wire floats
// high and reads DOOR OPEN. A broken sense line therefore refuses to start a run rather than
// silently claiming the door is shut.
//
// Keep this header free of <Arduino.h> so it stays native-compilable.
#pragma once

struct IDoorSensor {
  virtual ~IDoorSensor() = default;
  // True when the door is open (or the sense line is broken — see the polarity note above).
  virtual bool isOpen() const = 0;
};
