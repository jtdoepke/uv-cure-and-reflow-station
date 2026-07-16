#include "screen_router.h"

void ScreenRouter::define(int id, BuildFn build, void *ctx, bool cached, ResetFn reset) {
  if (id < 0 || id >= kMaxScreens) {
    return;
  }
  Def &d = defs_[id];
  d.build = build;
  d.reset = reset;
  d.ctx = ctx;
  d.cached = cached;
  d.cached_obj = nullptr;
}

bool ScreenRouter::isResident(int id) const {
  if (id < 0 || id >= kMaxScreens) {
    return false;
  }
  return defs_[id].cached_obj != nullptr;
}

void ScreenRouter::show(int id) {
  if (id < 0 || id >= kMaxScreens || defs_[id].build == nullptr) {
    return;
  }
  Def &d = defs_[id];

  lv_obj_t *scr;
  if (d.cached && d.cached_obj != nullptr) {
    scr = d.cached_obj; // cached hit — no rebuild
    if (d.reset != nullptr) {
      d.reset(d.ctx); // restore rebuild-default view-state so this re-show matches a fresh build
    }
  } else {
    scr = lv_obj_create(nullptr);
    d.build(d.ctx, scr);
    if (d.cached) {
      d.cached_obj = scr;
    }
  }

  lv_obj_t *prev_obj = current_obj_;
  const bool prev_cached = (current_ >= 0) && defs_[current_].cached;

  lv_screen_load(scr); // no load animation, so the swap is immediate and the old screen is inactive
  current_ = id;
  current_obj_ = scr;

  // Free the screen we just left only if it was create-on-demand; a cached one stays resident. The
  // guard also covers re-showing the current screen (prev_obj == scr → nothing to delete).
  if (prev_obj != nullptr && prev_obj != scr && !prev_cached) {
    lv_obj_delete(prev_obj);
  }
}
