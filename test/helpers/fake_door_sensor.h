// Scriptable fake for IDoorSensor — injected into ControllerRunPath under native_control so the
// §15 door-open behaviour is tested deterministically, no hardware. Header-only, shared via
// `#include "helpers/fake_door_sensor.h"`.
#pragma once

#include "IDoorSensor.h"

// Defaults CLOSED so a test that does not care about the door reads a runnable oven — the opposite
// of the *hardware* default, which fails safe to open (a broken sense line). That asymmetry is
// deliberate: the adapter's job is to be pessimistic about a wire that might be cut; a fake's job
// is to not make every unrelated test opt out of a door it never mentions.
struct FakeDoorSensor : IDoorSensor {
  bool open = false;
  mutable int reads = 0; // so a test can assert the run path actually consults it every tick

  bool isOpen() const override {
    ++reads;
    return open;
  }
};
