#include "confirm_dialog.h"

#include "theme.h"

namespace {

// The captured seam, heap-owned and freed when the dialog root is deleted (captureless thunks can
// hold no state, so it rides in each button's event user_data).
struct Seam {
  void (*on_confirm)(void *);
  void (*on_cancel)(void *);
  void *ud;
};

void confirm_thunk(lv_event_t *e) {
  auto *s = static_cast<Seam *>(lv_event_get_user_data(e));
  if (s->on_confirm != nullptr) {
    s->on_confirm(s->ud); // may rebuild the page and delete this dialog — s is not touched after
  }
}

void cancel_thunk(lv_event_t *e) {
  auto *s = static_cast<Seam *>(lv_event_get_user_data(e));
  if (s->on_cancel != nullptr) {
    s->on_cancel(s->ud);
  }
}

lv_obj_t *dialog_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, Seam *seam) {
  lv_obj_t *btn = lv_button_create(parent);
  theme::apply_secondary(btn);
  lv_obj_set_flex_grow(btn, 1);
  lv_obj_set_height(btn, theme::TOUCH_MIN); // the design guide's 10 mm floor
  lv_obj_t *label = lv_label_create(btn);
  lv_label_set_text(label, text);
  lv_obj_center(label);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, seam);
  return btn;
}

} // namespace

ConfirmDialog create_confirm_dialog(lv_obj_t *parent, const char *message,
                                    const char *confirm_label, void (*on_confirm)(void *),
                                    void (*on_cancel)(void *), void *user_data) {
  ConfirmDialog ui{};

  auto *seam = static_cast<Seam *>(lv_malloc(sizeof(Seam)));
  seam->on_confirm = on_confirm;
  seam->on_cancel = on_cancel;
  seam->ud = user_data;

  // Scrim: covers the whole parent, dimming the page beneath and swallowing taps that miss the
  // dialog (it is clickable so touches do not fall through to the disabled page).
  ui.root = lv_obj_create(parent);
  lv_obj_remove_style_all(ui.root);
  // FLOATING so the scrim overlays the page instead of being placed in its flex flow (an owning
  // page is a flex column — without this the full-size scrim would be laid out as the next column
  // item and pushed off-screen). Aligned to cover the whole parent.
  lv_obj_add_flag(ui.root, LV_OBJ_FLAG_FLOATING);
  lv_obj_set_size(ui.root, lv_pct(100), lv_pct(100));
  lv_obj_align(ui.root, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_set_style_bg_color(ui.root, theme::col(theme::BG), 0);
  lv_obj_set_style_bg_opa(ui.root, LV_OPA_70, 0);
  lv_obj_add_flag(ui.root, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(ui.root, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(
      ui.root,
      [](lv_event_t *e) {
        lv_free(lv_obj_get_user_data(static_cast<lv_obj_t *>(lv_event_get_target(e))));
      },
      LV_EVENT_DELETE, nullptr);
  lv_obj_set_user_data(ui.root, seam);

  // Centred dialog panel.
  ui.panel = lv_obj_create(ui.root);
  theme::apply_panel(ui.panel);
  lv_obj_set_width(ui.panel, lv_pct(86));
  lv_obj_set_height(ui.panel, LV_SIZE_CONTENT);
  lv_obj_center(ui.panel);
  lv_obj_set_flex_flow(ui.panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(ui.panel, theme::PAD_M, 0);
  lv_obj_set_style_pad_row(ui.panel, theme::GAP, 0);
  lv_obj_remove_flag(ui.panel, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *msg = lv_label_create(ui.panel);
  lv_label_set_text(msg, message);
  lv_obj_set_width(msg, lv_pct(100));
  lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);

  lv_obj_t *row = lv_obj_create(ui.panel);
  theme::apply_row(row);
  lv_obj_set_width(row, lv_pct(100));
  lv_obj_set_height(row, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);

  // Cancel first (the safe default sits left), then the named confirm verb. Neither is danger-red:
  // deleting a file is destructive but not an energizing hazard (§13/§22 reserve red for that).
  ui.btn_cancel = dialog_button(row, "Cancel", cancel_thunk, seam);
  ui.btn_confirm = dialog_button(row, confirm_label, confirm_thunk, seam);
  return ui;
}
