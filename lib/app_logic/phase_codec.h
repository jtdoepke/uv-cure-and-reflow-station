// phase_codec.h — convert the CYD's editable domain model (phase.h Phase / RecipeMode / FanMode)
// to and from the wire types (oven_Phase / oven_Profile / oven_Mode / oven_FanMode). Added with
// Wave R3 of the §2 "CYD is a UI remote" split (2026-07-17): the profile library now lives on the
// controller as typed wire messages (§9), so the editor keeps working on the ergonomic domain
// Phase[] and this codec bridges at the wire boundary — the same pattern recipe_compiler.h already
// uses to lower Phase -> Segment, kept apart so the editor/templates/compiler stay unchanged.
//
// Pure C++ over nanopb + phase.h — no LVGL, no Arduino — host-tested under native_logic_cyd.
#pragma once

#include <cstddef>
#include <cstring>

#include "oven.pb.h"
#include "phase.h"

namespace phase_codec {

// FanMode {Auto,On,Off} (phase.h) <-> oven_FanMode {AUTO,ON,OFF}. Same order by construction
// (proto FAN_MODE_* mirror the domain values), but map explicitly so a future reorder can't drift.
inline oven_FanMode fanToWire(FanMode f) {
  switch (f) {
  case FanMode::On:
    return oven_FanMode_FAN_MODE_ON;
  case FanMode::Off:
    return oven_FanMode_FAN_MODE_OFF;
  case FanMode::Auto:
  default:
    return oven_FanMode_FAN_MODE_AUTO;
  }
}
inline FanMode fanFromWire(oven_FanMode f) {
  switch (f) {
  case oven_FanMode_FAN_MODE_ON:
    return FanMode::On;
  case oven_FanMode_FAN_MODE_OFF:
    return FanMode::Off;
  case oven_FanMode_FAN_MODE_AUTO:
  default:
    return FanMode::Auto;
  }
}

// RecipeMode {Cure,Reflow} (phase.h) <-> oven_Mode {CURE=1,REFLOW=2}. NOT the same integer values
// (domain Cure=0/Reflow=1 vs wire CURE=1/REFLOW=2), so this map is load-bearing.
inline oven_Mode modeToWire(RecipeMode m) {
  return m == RecipeMode::Cure ? oven_Mode_MODE_CURE : oven_Mode_MODE_REFLOW;
}
inline RecipeMode modeFromWire(oven_Mode m) {
  return m == oven_Mode_MODE_CURE ? RecipeMode::Cure : RecipeMode::Reflow;
}

inline oven_Phase phaseToWire(const Phase &p) {
  oven_Phase w = oven_Phase_init_zero;
  std::strncpy(w.name, p.name, sizeof(w.name) - 1);
  w.target_c = p.targetC;
  w.ramp_s = p.rampSeconds;
  w.hold_s = p.holdSeconds;
  w.exposure_per_surface = p.exposurePerSurface;
  w.uv = p.uv;
  w.motor = p.motor;
  w.fan_mode = fanToWire(p.convFan);
  return w;
}

inline Phase phaseFromWire(const oven_Phase &w) {
  Phase p;
  std::strncpy(p.name, w.name, kPhaseNameCap - 1);
  p.name[kPhaseNameCap - 1] = '\0';
  p.targetC = w.target_c;
  p.rampSeconds = w.ramp_s;
  p.holdSeconds = w.hold_s;
  p.exposurePerSurface = w.exposure_per_surface;
  p.uv = w.uv;
  p.motor = w.motor;
  p.convFan = fanFromWire(w.fan_mode);
  return p;
}

// Build a wire Profile from an authored Phase[]. `stock` is normally false from the editor (a
// Save-as clears it); the controller stamps the authoritative mode on save, but we fill it anyway
// for a well-formed message. Clamps the phase count to the wire array bound.
inline oven_Profile profileToWire(const char *name, RecipeMode mode, bool stock,
                                  const Phase *phases, size_t count) {
  oven_Profile w = oven_Profile_init_zero;
  std::strncpy(w.name, name, sizeof(w.name) - 1);
  w.mode = modeToWire(mode);
  w.stock = stock;
  const size_t cap = sizeof(w.phases) / sizeof(w.phases[0]);
  if (count > cap) {
    count = cap;
  }
  w.phases_count = static_cast<pb_size_t>(count);
  for (size_t i = 0; i < count; ++i) {
    w.phases[i] = phaseToWire(phases[i]);
  }
  return w;
}

// Extract a wire Profile's phases into a domain Phase[] (writing at most `cap`); returns the count
// written. Every domain Phase is NUL-terminated by phaseFromWire (nanopb already terminates, but
// defense-in-depth on the untrusted-fetch path).
inline size_t phasesFromWire(const oven_Profile &w, Phase *out, size_t cap) {
  size_t n = w.phases_count;
  const size_t bound = sizeof(w.phases) / sizeof(w.phases[0]);
  if (n > bound) {
    n = bound;
  }
  if (n > cap) {
    n = cap;
  }
  for (size_t i = 0; i < n; ++i) {
    out[i] = phaseFromWire(w.phases[i]);
  }
  return n;
}

} // namespace phase_codec
