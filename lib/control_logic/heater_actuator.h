// HeaterActuator — time-proportioning ("slow PWM") for the heater SSR (design.md §11).
//
// A zero-cross SSR can only switch at AC zero crossings, so heater "power" is realized
// by holding the switch on for `duty × window` over a ~1 s window. That algorithm lives
// here in portable logic (not the port) so it's host-testable behind IHeaterSwitch +
// IClock with a FakeClock and a recording switch. The PID + feedforward (§5) drive
// setDuty(); the SafetySupervisor (§4) calls forceOff() — the actuator never overrides
// safety, and a hung loop is caught by the hardware watchdog (reset -> pull-down -> OFF),
// not by this class.
//
// The switch and clock are injected by reference and must outlive this object.
#pragma once

#include <cstdint>

#include "IClock.h"
#include "IHeaterSwitch.h"

class HeaterActuator {
public:
  struct Config {
    uint32_t windowMs = 1000; // proportioning period
    uint32_t minOnMs = 50;    // duties below this snap to full-OFF (SSR/thermal floor)
    uint32_t minOffMs = 50;   // duties above (window-minOff) snap to full-ON
  };

  HeaterActuator(IHeaterSwitch &sw, IClock &clock, Config cfg)
      : sw_(sw), clock_(clock), cfg_(cfg) {}

  // Convenience overload with default Config. (A `Config cfg = Config{}` default
  // argument can't be used here: it would reference Config's default member
  // initializers before the enclosing class is complete.)
  HeaterActuator(IHeaterSwitch &sw, IClock &clock) : HeaterActuator(sw, clock, Config{}) {}

  // The duty currently in force, 0..1. Read *after* SafetySupervisor::tick() to get what the
  // heater is actually doing rather than what the control loop asked for: forceOff() zeroes
  // this, so a safety cut shows up here. Telemetry (§9) reports it.
  float duty() const { return duty_; }

  // Commanded duty from the control loop; clamped to 0..1 and stored. Takes effect at
  // the next window latch (tick()), so a mid-window change can't glitch the output.
  // The lower test is `!(d0to1 > 0.0F)` rather than `d0to1 < 0.0F` so NaN maps to OFF:
  // a NaN duty would otherwise survive both compares and reach latchOnMs()'s
  // static_cast<uint32_t>(NaN) — UB that can latch the heater full-ON.
  void setDuty(float d0to1) { duty_ = !(d0to1 > 0.0F) ? 0.0F : (d0to1 > 1.0F ? 1.0F : d0to1); }

  // Call every control loop. Starts a fresh window every windowMs (latching that
  // window's on-time from the stored duty), then drives the switch on/off within it.
  void tick() {
    uint32_t now = clock_.millis();
    if (!started_) {
      started_ = true;
      windowStart_ = now;
      onMs_ = latchOnMs();
    } else if (static_cast<uint32_t>(now - windowStart_) >= cfg_.windowMs) {
      // Advance by whole windows so a late tick doesn't drift the phase.
      uint32_t elapsed = static_cast<uint32_t>(now - windowStart_);
      windowStart_ += (elapsed / cfg_.windowMs) * cfg_.windowMs;
      onMs_ = latchOnMs();
    }
    bool want = static_cast<uint32_t>(now - windowStart_) < onMs_;
    drive(want);
  }

  // Safety override: immediate OFF, duty := 0, and hold OFF for the rest of this
  // window (does not wait for the next boundary).
  void forceOff() {
    duty_ = 0.0F;
    onMs_ = 0;
    drive(false);
  }

private:
  // On-time for a window, with min-on/min-off snapping to avoid sliver pulses.
  uint32_t latchOnMs() const {
    uint32_t on = static_cast<uint32_t>(duty_ * static_cast<float>(cfg_.windowMs) + 0.5F);
    if (on > cfg_.windowMs) {
      on = cfg_.windowMs;
    }
    if (on < cfg_.minOnMs) {
      return 0;
    }
    if (on > cfg_.windowMs - cfg_.minOffMs) {
      return cfg_.windowMs;
    }
    return on;
  }

  // Only touch the switch on an actual state change (avoid redundant GPIO writes).
  void drive(bool on) {
    if (on != on_) {
      on_ = on;
      sw_.set(on);
    }
  }

  IHeaterSwitch &sw_;
  IClock &clock_;
  Config cfg_;
  float duty_ = 0.0F;
  uint32_t windowStart_ = 0;
  uint32_t onMs_ = 0;
  bool started_ = false;
  bool on_ = false;
};
