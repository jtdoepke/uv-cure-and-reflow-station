#include "hold_button.h"

#include "theme.h"

// Per-widget state: the timing model + the arm callback + the fill overlay + the poll timer. Owned
// by the button via user_data, freed on the button's LV_EVENT_DELETE (which also deletes the timer
// — the one safe place, since the arm callback may itself delete the whole widget tree).
namespace {
struct HoldCtx {
  HoldButtonModel model;
  void (*on_arm)(void *) = nullptr;
  void *ud = nullptr;
  lv_obj_t *fill = nullptr;
  lv_timer_t *timer = nullptr;
};

// The fill sweeps left-to-right by widening from 0 to full as progress() goes 0→1.
void set_fill(lv_obj_t *fill, float progress01) {
  int32_t pct = static_cast<int32_t>(progress01 * 100.0f);
  if (pct < 0) {
    pct = 0;
  } else if (pct > 100) {
    pct = 100;
  }
  lv_obj_set_width(fill, lv_pct(pct));
}

// ~30 ms poll: resize the fill from progress(), and fire once at the arm edge. LVGL tolerates a
// timer being deleted from within its own callback, so on_arm() deleting the widget (navigation) is
// safe — ctx is simply not touched after the call.
void tick_cb(lv_timer_t *timer) {
  auto *ctx = static_cast<HoldCtx *>(lv_timer_get_user_data(timer));
  const uint32_t now = lv_tick_get();
  set_fill(ctx->fill, ctx->model.progress(now));
  if (ctx->model.poll(now)) {
    set_fill(ctx->fill, 1.0f);
    if (ctx->on_arm != nullptr) {
      ctx->on_arm(ctx->ud); // may delete this widget → timer + ctx freed via the DELETE handler
    }
    return; // do NOT touch ctx after on_arm — the widget may be gone
  }
}

void ensure_timer(HoldCtx *ctx) {
  if (ctx->timer == nullptr) {
    ctx->timer = lv_timer_create(tick_cb, 30, ctx);
  }
}

void stop_timer(HoldCtx *ctx) {
  if (ctx->timer != nullptr) {
    lv_timer_delete(ctx->timer);
    ctx->timer = nullptr;
  }
}

void on_pressed(lv_event_t *e) {
  auto *ctx = static_cast<HoldCtx *>(lv_event_get_user_data(e));
  ctx->model.press(lv_tick_get());
  ensure_timer(ctx);
}

// Finger up or slid off the button: cancel the hold and reset the fill (armed latches, so a
// completed hold that already fired is unaffected).
void on_release(lv_event_t *e) {
  auto *ctx = static_cast<HoldCtx *>(lv_event_get_user_data(e));
  ctx->model.release();
  stop_timer(ctx);
  set_fill(ctx->fill, 0.0f);
}

void on_delete(lv_event_t *e) {
  auto *ctx = static_cast<HoldCtx *>(lv_event_get_user_data(e));
  stop_timer(ctx);
  delete ctx;
}
} // namespace

HoldButton create_hold_button(lv_obj_t *parent, const char *label, uint32_t hold_ms,
                              void (*on_arm)(void *), void *ud) {
  auto *ctx = new HoldCtx();
  ctx->model.configure(hold_ms);
  ctx->on_arm = on_arm;
  ctx->ud = ud;

  lv_obj_t *btn = lv_button_create(parent);
  theme::apply_mode_tile(btn); // the big primary commit action
  lv_obj_set_width(btn, lv_pct(100));
  lv_obj_set_height(btn, theme::SECONDARY_H);
  lv_obj_set_style_pad_all(btn, 0, 0);        // let the fill reach the edges
  lv_obj_set_style_clip_corner(btn, true, 0); // clip the fill to the button's rounded corners

  // The progress fill: a translucent-white overlay pinned to the left edge, full height, its width
  // growing 0→100% as the hold completes. Non-interactive (presses pass to the button), and behind
  // the label (created after it, so the label draws on top).
  lv_obj_t *fill = lv_obj_create(btn);
  lv_obj_remove_style_all(fill);
  lv_obj_set_height(fill, lv_pct(100));
  lv_obj_set_width(fill, lv_pct(0));
  lv_obj_align(fill, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_set_style_bg_color(fill, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(fill, LV_OPA_30, 0);
  lv_obj_remove_flag(fill, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t *lbl = lv_label_create(btn);
  lv_label_set_text(lbl, label);
  lv_obj_center(lbl);

  ctx->fill = fill;
  lv_obj_add_event_cb(btn, on_pressed, LV_EVENT_PRESSED, ctx);
  lv_obj_add_event_cb(btn, on_release, LV_EVENT_RELEASED, ctx);
  lv_obj_add_event_cb(btn, on_release, LV_EVENT_PRESS_LOST, ctx);
  lv_obj_add_event_cb(btn, on_delete, LV_EVENT_DELETE, ctx);

  return HoldButton{btn, fill, lbl};
}
