#include "main_ui.h"

// Tap counter lives in the click handler's user_data-referenced label. We keep the count
// as a static local so the demo needs no extra state object; when the real mode UI lands
// this becomes a proper model (an lv_subject_t or an app_logic object).
static void btn_event_cb(lv_event_t *e) {
  static int count = 0;
  lv_obj_t *label = (lv_obj_t *)lv_event_get_user_data(e);
  lv_label_set_text_fmt(label, "Touched %d", ++count);
}

MainUi create_main_ui(lv_obj_t *parent) {
  MainUi ui{};

  ui.title = lv_label_create(parent);
  lv_label_set_text(ui.title, "Hello CYD!");
  lv_obj_align(ui.title, LV_ALIGN_TOP_MID, 0, 20);

  ui.button = lv_button_create(parent);
  lv_obj_center(ui.button);
  ui.btn_label = lv_label_create(ui.button);
  lv_label_set_text(ui.btn_label, "Tap me");
  lv_obj_add_event_cb(ui.button, btn_event_cb, LV_EVENT_CLICKED, ui.btn_label);

  return ui;
}
