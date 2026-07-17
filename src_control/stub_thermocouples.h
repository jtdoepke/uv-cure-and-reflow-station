// StubThermocouples — a placeholder IThermocouples for the controller firmware until the
// real TC front-end adapter lands (backlog D4: MAX318xx / analog-amp channel bring-up).
//
// It reports NO usable channels, so it can never masquerade as a real temperature: every
// reading is flagged fault, and wallCount() is 0. SafetySupervisor's L3 checks only consult
// it while a run is armed, and nothing arms a run on-device yet (the ProfileExecutor→PID
// wiring rides in with D4's real sensor). If a run ever were armed against this stub, the
// supervisor would immediately trip SENSOR_FAULT — the fail-safe reading, which is the point.
//
// This mirrors A4a's posture (logic host-tested with fakes; the real Esp32* adapter deferred
// to the hardware step). Delete when D4's adapter replaces it in main.cpp.
#pragma once

#include "IThermocouples.h"

struct StubThermocouples : IThermocouples {
  TcReading workpiece() const override { return {0.0F, true}; }
  int wallCount() const override { return 0; }
  TcReading wall(int) const override { return {0.0F, true}; }
};
