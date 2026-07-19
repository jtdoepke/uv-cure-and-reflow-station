// hold_button — a press-and-hold "arm" control (design.md §19; backlog C6b). A deliberate,
// hard-to-trip-by-accident commit gesture for the one irreversible action on the panel: starting a
// run (and, later, resuming a paused cure). You press and HOLD; a ring fills over ~hold_ms; only at
// full does it fire. Lifting early cancels with nothing done — the §19 "easy to cancel, deliberate
// to commit" rule, the inverse of a tap.
//
// The timing is a pure model (HoldButtonModel) so it is host-tested without LVGL: the widget just
// feeds it lv_tick_get() and paints the ring from progress(). LVGL-only widget half; the model is
// plain C++. Compiles for firmware and the native_ui_cyd / native_sim host targets.
#pragma once

#include <cstdint>

#include <lvgl.h>

// The press-and-hold timing, independent of any widget. `now` is a millisecond clock (monotonic
// within a press); the caller supplies it (lv_tick_get() in the widget, a fake in tests).
struct HoldButtonModel {
  uint32_t hold_ms = 1500; // full-arm dwell; the §19 "deliberate" duration
  bool holding = false;
  bool armed = false;
  uint32_t start_ms = 0;

  void configure(uint32_t ms) { hold_ms = ms; }
  void reset() {
    holding = false;
    armed = false;
  }

  // Finger down: begin (or restart) the hold.
  void press(uint32_t now) {
    holding = true;
    armed = false;
    start_ms = now;
  }
  // Finger up / slid off: cancel — unless the hold already completed (armed latches).
  void release() { holding = false; }

  // Fill fraction [0,1] for the ring. 0 when idle, 1 once armed or the dwell is met.
  float progress(uint32_t now) const {
    if (armed) {
      return 1.0f;
    }
    if (!holding) {
      return 0.0f;
    }
    if (hold_ms == 0) {
      return 1.0f;
    }
    const uint32_t elapsed = now - start_ms;
    if (elapsed >= hold_ms) {
      return 1.0f;
    }
    return static_cast<float>(elapsed) / static_cast<float>(hold_ms);
  }

  // Drive the hold; call each tick while a press is live. Returns true EXACTLY ONCE, at the instant
  // the dwell completes (the commit edge). After that the press is consumed (holding clears).
  bool poll(uint32_t now) {
    if (holding && !armed && (now - start_ms) >= hold_ms) {
      armed = true;
      holding = false;
      return true;
    }
    return false;
  }
};

// Handles into the built widget so callers/tests can inspect or gate it.
struct HoldButton {
  lv_obj_t *root; // the pressable button (gate it via LV_STATE_DISABLED / the CLICKABLE flag)
  lv_obj_t *arc;  // the fill ring (0..100)
  lv_obj_t *label;
};

// Build a hold-to-arm button under `parent` reading `label`. Press-and-hold for `hold_ms` fires
// `on_arm(ud)` once; lifting early cancels. Requires lv_init(); the widget reads lv_tick_get(), so
// the host loop must advance ticks (the sim/tests do).
HoldButton create_hold_button(lv_obj_t *parent, const char *label, uint32_t hold_ms,
                              void (*on_arm)(void *ud), void *ud);
