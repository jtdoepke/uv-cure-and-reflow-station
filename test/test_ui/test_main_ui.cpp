// native_ui suite — LVGL 9.5 headless UI test. LVGL runs on the host with LV_USE_TEST=1;
// an in-memory dummy display + simulated pointer drive the real create_main_ui() widgets.
// No board, no pixels on glass. LovyanGFX is not linked (lib_ignore in the env).
#include <unity.h>
#include <lvgl.h>
#include "src/debugging/test/lv_test.h" // lv_test_display_create / mouse / wait (gated by LV_USE_TEST)

#include "main_ui.h"

void setUp(void) {
  lv_init();
  lv_test_display_create(320, 240);
  lv_test_indev_create_all();
}

void tearDown(void) {
  lv_deinit(); // fresh LVGL per test so static widget state doesn't leak between runs
}

void test_button_click_increments_label(void) {
  MainUi ui = create_main_ui(lv_screen_active());
  lv_obj_update_layout(lv_screen_active());

  lv_area_t coords;
  lv_obj_get_coords(ui.button, &coords);
  int32_t cx = (coords.x1 + coords.x2) / 2;
  int32_t cy = (coords.y1 + coords.y2) / 2;

  lv_test_mouse_click_at(cx, cy);
  lv_test_wait(50);
  TEST_ASSERT_EQUAL_STRING("Touched 1", lv_label_get_text(ui.btn_label));

  lv_test_mouse_click_at(cx, cy);
  lv_test_wait(50);
  TEST_ASSERT_EQUAL_STRING("Touched 2", lv_label_get_text(ui.btn_label));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_button_click_increments_label);
  return UNITY_END();
}
