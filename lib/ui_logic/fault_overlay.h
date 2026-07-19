// fault_overlay — the §22 fault / alarm modal (backlog C8).
//
// The one screen that appears UNBIDDEN, over any other screen including a sleeping one. Its job is
// not control: by the time it draws, the controller has already cut heater + UV (§4 L1–L3, §9
// `Fault`), so there is nothing left to stop and no STOP button. It is an alarm plus an
// acknowledgment.
//
// Lives on `lv_layer_top()` rather than in the ScreenRouter, which is what makes "over ANY screen"
// true: the router owns one screen at a time and deletes create-on-demand screens as you navigate,
// so a modal parented to a screen would die with it. The top layer also survives a sleep-wake
// rebuild. While the overlay is up the layer takes LV_OBJ_FLAG_CLICKABLE, which is what actually
// makes it modal — LVGL's top layer passes touches through to the screen beneath otherwise, and a
// fault modal you can tap past is not a modal.
//
// State lives in B7's FaultController (`lib/app_logic`) — the latch, the severity ordering, the
// `+N` supersede and the ack routing are all host-tested there and none of it is repeated here.
// This file is only the view plus the poll that diffs the controller's `updatedAtMs` into it.
//
// ON THE MISSING VIEW-MODEL: §22 specifies a `FaultViewModel` owning `lv_subject_t`s, "so no `lv_`
// calls happen off the UI task". That indirection is solving a threading hazard this firmware does
// not have — the CYD services its link from `loop()`, the same task that runs `lv_timer_handler()`
// (there is no `xTaskCreate` in `src_cyd/main.cpp`) — so a subject here would be a publish and an
// immediate synchronous read by the only subscriber. The rule it protects still stands and is worth
// restating: if the link ever moves to its own task (§7/B8·2 contemplates exactly that for SD
// logging), the marshal goes HERE, and `poll()` is the seam it goes in.
//
// LVGL-only; compiles for firmware and the native_ui_cyd / native_sim host targets.
#pragma once

#include <cstdint>

#include <lvgl.h>

#include "fault_controller.h"
#include "fault_table.h"

class FaultOverlay {
public:
  // Bind to the latch. The controller must outlive this overlay.
  void begin(FaultController &fc);

  // Drive from the loop, after the link has been serviced and the latch ticked. Shows the modal on
  // a new/updated fault, keeps it up until acknowledged (§22: never auto-dismiss, even if the
  // condition clears), and refreshes the live reading. Cheap when nothing changed: it compares the
  // latch's `updatedAtMs` + active flag and returns.
  void poll();

  // Acknowledge — the gesture target, also directly callable by tests. Dismisses the overlay and
  // reports where the operator should land (§22: the Run Summary if a run was active, else Home).
  // Always allowed: it dismisses the ALARM, not the hazard, which is already handled.
  void acknowledge();

  // Fired on acknowledge with the route the latch chose. The composition root maps it to a screen.
  void setAckHandler(void (*cb)(void *user_data, AckRoute route), void *user_data);

  bool visible() const { return root_ != nullptr; }
  AckRoute lastRoute() const { return last_route_; }
  // The code currently displayed (FAULT_NONE when hidden) — for tests and the RGB-LED/buzzer
  // hook. Raw wire int, not the enum: an out-of-enum code is reachable and holding it as the enum
  // type is UB (fault_table.h).
  fault_table::FaultCodeWire shownCode() const { return shown_code_; }

private:
  void build(const FaultState &s);
  void teardown();

  FaultController *fc_ = nullptr;
  lv_obj_t *root_ = nullptr;     // the modal panel on lv_layer_top
  lv_obj_t *temp_lbl_ = nullptr; // the live chamber reading, refreshed in place
  uint32_t shown_at_ = 0;        // the latch's updatedAtMs we last built for
  bool shown_ = false;
  fault_table::FaultCodeWire shown_code_ = oven_FaultCode_FAULT_NONE;
  AckRoute last_route_ = AckRoute::None;
  int last_temp_ = INT32_MIN;

  void (*on_ack_)(void *, AckRoute) = nullptr;
  void *ack_ud_ = nullptr;

  friend struct FaultThunks;
};
