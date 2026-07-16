// A tiny hub-and-spoke screen manager (C4/C6). It owns each screen's lv_obj lifetime and its
// memory policy: create-on-demand (deleted when navigated away from — the §24 default for this
// PSRAM-less board) vs cached (built once and kept resident, so re-showing it is an lv_screen_load,
// not a rebuild — the Home-screen win, see src_cyd/main.cpp / perf/baseline/device-35.md).
//
// A cached screen may register a reset-on-show hook that restores its rebuild-default view-state
// (scroll position, selection, sub-page) before each re-show, so a cached re-show is
// pixel-identical to a fresh build. A screen is only safe to cache if (1) all its live values are
// subject-bound and (2) any view-state a rebuild would reset is restored by that hook (see the
// ui-development architecture.md cacheability rule). Home needs no hook (it is stateless); a
// stateful screen does.
//
// Board-blind, LVGL-only: compiles for firmware and the native_ui_cyd host target, and holds no
// state beyond the screen table — the widget trees live in the LVGL pool as usual.
#pragma once

#include <lvgl.h>

class ScreenRouter {
public:
  static constexpr int kMaxScreens = 8;

  // Populate `scr` — a fresh lv_obj screen the router created and owns — with the screen's widget
  // tree. Do NOT create the screen object or call lv_screen_load; the router does both.
  using BuildFn = void (*)(void *ctx, lv_obj_t *scr);
  // Restore a cached screen to its rebuild-default view-state, called before each cached re-show.
  // Optional: pass nullptr for a stateless screen (nothing a rebuild would reset).
  using ResetFn = void (*)(void *ctx);

  // Register screen `id` (0..kMaxScreens-1). `cached` pins it resident; `reset` runs on each cached
  // re-show. `ctx` is passed back to both callbacks. Re-defining an id drops its cached instance
  // reference (the caller owns any teardown of an already-built screen — normally you define once
  // at startup).
  void define(int id, BuildFn build, void *ctx, bool cached, ResetFn reset = nullptr);

  // Switch to screen `id`: load its cached instance (running reset-on-show) or build it fresh, make
  // it active, and delete the screen we left if that one was create-on-demand. Re-showing the
  // current screen re-runs its reset hook (cached) and never deletes it.
  void show(int id);

  int current() const { return current_; }
  lv_obj_t *currentObj() const { return current_obj_; }
  // True if screen `id` is cached and currently built/resident (for tests + inspection).
  bool isResident(int id) const;

private:
  struct Def {
    BuildFn build = nullptr;
    ResetFn reset = nullptr;
    void *ctx = nullptr;
    bool cached = false;
    lv_obj_t *cached_obj = nullptr; // resident instance of a cached screen once built
  };
  Def defs_[kMaxScreens];
  int current_ = -1;
  lv_obj_t *current_obj_ = nullptr;
};
