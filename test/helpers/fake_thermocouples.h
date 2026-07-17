// Programmable fake for IThermocouples — injected into the profile executor (A6) and
// the control loop (A5) under the native_control env so a temperature trajectory can
// be driven by hand alongside FakeClock, no hardware. Header-only, shared via
// `#include "helpers/fake_thermocouples.h"`.
#pragma once

#include "IThermocouples.h"

// Holds the current reading for the workpiece and a small fixed wall array; a test
// sets `.workpieceC` / `.wallC[i]` (and the matching `*Fault` flags) between calls to
// the code under test, the way FakeClock is advanced by hand.
struct FakeThermocouples : IThermocouples {
  static constexpr int kWalls = 4; // matches oven.proto wall_temp max_count

  float workpieceC = 25.0f;
  bool workpieceFault = false;
  float wallC[kWalls] = {25.0f, 25.0f, 25.0f, 25.0f};
  bool wallFault[kWalls] = {false, false, false, false};
  int walls = kWalls;

  TcReading workpiece() const override { return {workpieceC, workpieceFault}; }
  int wallCount() const override { return walls; }
  TcReading wall(int index) const override {
    if (index < 0 || index >= walls) {
      return {0.0f, true};
    }
    return {wallC[index], wallFault[index]};
  }

  // Convenience: set every channel to one temperature (fault-free).
  void setAll(float c) {
    workpieceC = c;
    workpieceFault = false;
    for (int i = 0; i < kWalls; ++i) {
      wallC[i] = c;
      wallFault[i] = false;
    }
  }
};
