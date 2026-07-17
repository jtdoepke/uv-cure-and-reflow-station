// IThermocouples — the controller's temperature-input port (design.md §5, §6, §11).
//
// The control loop (A5) and the profile executor (A6) read the oven's temperature
// through this port so they are host-testable against a FakeThermocouples with a
// hand-driven trajectory, exactly as timeouts are tested against a FakeClock. The
// production adapter (D4) wraps the thermocouple front-end IC (MAX31855/56-class);
// its exact channel count and open/short detection are the §10 "Thermocouples" open
// question — but the *logic-facing shape* is already decided here (§11) and mirrored
// by the wire: Telemetry carries `repeated float wall_temp` + `float work_temp`.
//
// Two sensor families (design.md §5 "the mode's control sensor"): the **workpiece**
// TC is the reflow control sensor (a probe on the board), and the **wall** array is
// the chamber-wall temperature — the cure control sensor (a good air proxy at 80 °C)
// and the L3 high-limit input. Each reading carries a `fault` flag: an open or
// shorted junction reads true, and blind control is a stop condition (§4), so a
// consumer must treat a faulted control channel as "safe", never as a temperature.
//
// Deliberately just readback — no policy. Which channel is the control sensor for a
// given run is the caller's choice (by mode), keeping the executor sensor-agnostic
// (§5). Keep this header free of <Arduino.h> so it stays native-compilable.
#pragma once

// One channel's reading. `fault` true => celsius is not trustworthy (open/short/
// out-of-range at the front end); the value is left unspecified in that case.
struct TcReading {
  float celsius;
  bool fault;
};

struct IThermocouples {
  virtual ~IThermocouples() = default;

  // The workpiece probe — the reflow control sensor (design.md §5/§6).
  virtual TcReading workpiece() const = 0;

  // The chamber-wall array: the cure control sensor and the L3 high-limit input.
  virtual int wallCount() const = 0;
  virtual TcReading wall(int index) const = 0; // index in [0, wallCount())
};
