// SimThermocouples — a plant-backed IThermocouples for the bench simulator (backlog A10).
//
// The bench sim swaps this in for the real thermocouple front-end so the controller reads
// *synthetic* readings from the OvenPlant model. It reads the plant every call: the workpiece
// channel from the lagged board node, and the wall array from the chamber wall/air node (§6:
// fan-mixed walls are a good air proxy). Owns no physics — the caller (main.cpp on device, the host
// test) steps the referenced OvenPlant with the commanded outputs each loop, then the run path and
// SafetySupervisor read the results back through this port.
//
// Lives in lib/ (not src_control) because it is pure logic — no Arduino, board-agnostic — so the
// host closed-loop test drives the identical adapter the CONTROL_SIM firmware does.
//
// Deliberately deterministic (no RNG): fixed per-channel offsets model the chamber's spatial map,
// and optional front-end quantization models a MAX31855-class part's 0.25 °C step — both settable
// so a test can dial them in. Fault-injection flags drive the SENSOR_FAULT / TC_IMPLAUSIBLE paths
// (A4b/A6): set a channel's fault true to simulate an open/short.
//
// SAFETY NOTE: fabricates sensor readings — CONTROL_SIM bench build / host tests only; never at a
// real oven. Header-only; holds a reference to the OvenPlant, which must outlive it.
#pragma once

#include "IThermocouples.h"
#include "oven_plant.h"

class SimThermocouples : public IThermocouples {
public:
  static constexpr int kMaxWalls = 4; // matches oven.proto wall_temp max_count / FakeThermocouples

  explicit SimThermocouples(const OvenPlant &plant) : plant_(plant) {}

  TcReading workpiece() const override {
    if (workpieceFault) {
      return {0.0f, true};
    }
    return {condition(plant_.workpieceTempC()), false};
  }

  int wallCount() const override { return walls; }

  TcReading wall(int index) const override {
    if (index < 0 || index >= walls) {
      return {0.0f, true};
    }
    if (wallFault[index]) {
      return {0.0f, true};
    }
    return {condition(plant_.wallTempC() + wallOffsetC[index]), false};
  }

  // --- Injectable measurement realism / faults (all default to "clean") ---
  int walls = kMaxWalls;                                    // usable wall-channel count
  float wallOffsetC[kMaxWalls] = {0.0f, 0.0f, 0.0f, 0.0f};  // fixed per-channel spatial offset
  bool wallFault[kMaxWalls] = {false, false, false, false}; // open/short injection per channel
  bool workpieceFault = false;                              // open/short injection, workpiece
  float quantizeC = 0.0f; // >0 rounds to this step (e.g. 0.25 for a MAX31855-class front end)

private:
  // Apply optional front-end quantization to a plant temperature.
  float condition(float c) const {
    if (quantizeC > 0.0f) {
      const float k = c / quantizeC;
      const long r = static_cast<long>(k >= 0.0f ? k + 0.5f : k - 0.5f);
      return static_cast<float>(r) * quantizeC;
    }
    return c;
  }

  const OvenPlant &plant_;
};
