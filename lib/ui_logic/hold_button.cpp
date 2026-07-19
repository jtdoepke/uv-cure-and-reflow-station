#include "hold_button.h"

#include "theme.h"

// Per-widget state: the timing model + the arm callback + the fill ring + the poll timer. Owned by
// the button via user_data, freed on the button's LV_EVENT_DELETE (which also deletes the timer —
// the one safe place, since the arm callback may itself delete the whole widget tree).
namespace {
struct HoldCtx {
  HoldButtonModel model;
  void (*on_arm)(void *) = nullptr;
  void *ud = nullptr;
  lv_obj_t *arc = nullptr;
  lv_timer_t *timer = nullptr;
};

// ~30 ms poll: repaint the ring from progress(), and fire once at the arm edge. LVGL tolerates a
// timer being deleted from within its own callback, so on_arm() deleting the widget (navigation) is
// safe — ctx is simply not touched after the call.
void tick_cb(lv_timer_t *timer) {
  auto *ctx = static_cast<HoldCtx *>(lv_timer_get_user_data(timer));
  const uint32_t now = lv_tick_get();
  lv_arc_set_value(ctx->arc, static_cast<int32_t>(ctx->model.progress(now) * 100.0f));
  if (ctx->model.poll(now)) {
    lv_arc_set_value(ctx->arc, 100);
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

// Finger up or slid off the button: cancel the hold and reset the ring (armed latches, so a
// completed hold that already fired is unaffected).
void on_release(lv_event_t *e) {
  auto *ctx = static_cast<HoldCtx *>(lv_event_get_user_data(e));
  ctx->model.release();
  stop_timer(ctx);
  lv_arc_set_value(ctx->arc, 0);
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

  // The fill ring, centred and non-interactive (presses pass through to the button). It grows
  // 0→360° as the hold completes — the visible "keep holding" feedback the §19 gesture needs.
  lv_obj_t *arc = lv_arc_create(btn);
  const int32_t d = theme::SECONDARY_H - theme::PAD_S;
  lv_obj_set_size(arc, d, d);
  lv_obj_align(arc, LV_ALIGN_LEFT_MID, theme::PAD_S / 2, 0);
  lv_arc_set_rotation(arc, 270);
  lv_arc_set_bg_angles(arc, 0, 360);
  lv_arc_set_range(arc, 0, 100);
  lv_arc_set_value(arc, 0);
  lv_arc_set_mode(arc, LV_ARC_MODE_NORMAL);
  lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_style(arc, nullptr, LV_PART_KNOB); // decorative — no drag knob
  lv_obj_set_style_arc_color(arc, theme::col(theme::ACCENT), LV_PART_INDICATOR);
  lv_obj_set_style_arc_opa(arc, LV_OPA_30, LV_PART_MAIN);

  lv_obj_t *lbl = lv_label_create(btn);
  lv_label_set_text(lbl, label);
  lv_obj_center(lbl);

  ctx->arc = arc;
  lv_obj_add_event_cb(btn, on_pressed, LV_EVENT_PRESSED, ctx);
  lv_obj_add_event_cb(btn, on_release, LV_EVENT_RELEASED, ctx);
  lv_obj_add_event_cb(btn, on_release, LV_EVENT_PRESS_LOST, ctx);
  lv_obj_add_event_cb(btn, on_delete, LV_EVENT_DELETE, ctx);

  return HoldButton{btn, arc, lbl};
}
