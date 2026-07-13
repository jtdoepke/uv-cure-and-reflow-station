// native_ui_cyd suite — LVGL 9.5 headless test of the selectable list (§23/§24), the glove-safe
// ▲/▼-highlight + Open widget. Asserts the wiring: init lands on the first enabled row, the Up/Down
// footer skips disabled ("coming soon") rows and disables at the ends, tapping a row highlights it,
// and Open fires the seam with the highlighted index.
#include <unity.h>
#include <lvgl.h>
#include "src/debugging/test/lv_test.h"

#include "selectable_list.h"

// File-static model: its lv_subject_t must outlive lv_deinit() (rows unlink observers at teardown).
static SelectableListModel model;

static bool open_called;
static int opened_index;
static void on_open(int index, void *) {
  open_called = true;
  opened_index = index;
}

// A hub-shaped list with a disabled row in the middle (row 2).
static const SelectableListItem kItems[] = {
    {"Display & units", nullptr, true},   // 0
    {"Temperature limits", "caps", true}, // 1
    {"Network (WiFi)", "soon", false},    // 2 (disabled)
    {"About", nullptr, true},             // 3
};
static constexpr int kCount = 4;

void setUp(void) {
  lv_init();
  lv_test_display_create(320, 240);
  lv_test_indev_create_all();
  open_called = false;
  opened_index = -1;
}

void tearDown(void) {
  lv_deinit();
}

static void click_center(lv_obj_t *obj) {
  lv_obj_update_layout(lv_screen_active());
  lv_area_t c;
  lv_obj_get_coords(obj, &c);
  lv_test_mouse_click_at((c.x1 + c.x2) / 2, (c.y1 + c.y2) / 2);
  lv_test_wait(50);
}

// --- Model-level ---

void test_init_selects_first_enabled(void) {
  model.init(kItems, kCount);
  TEST_ASSERT_EQUAL_INT(0, model.selected());
  TEST_ASSERT_TRUE(model.atFirstEnabled());
  TEST_ASSERT_FALSE(model.atLastEnabled());
}

void test_init_skips_leading_disabled(void) {
  static const SelectableListItem items[] = {
      {"Coming soon", nullptr, false},
      {"Real", nullptr, true},
  };
  model.init(items, 2);
  TEST_ASSERT_EQUAL_INT(1, model.selected());
}

void test_move_skips_disabled_and_saturates(void) {
  model.init(kItems, kCount);
  model.moveDown(); // 0 -> 1
  TEST_ASSERT_EQUAL_INT(1, model.selected());
  model.moveDown(); // 1 -> skip 2 -> 3
  TEST_ASSERT_EQUAL_INT(3, model.selected());
  TEST_ASSERT_TRUE(model.atLastEnabled());
  model.moveDown(); // at last enabled, stays
  TEST_ASSERT_EQUAL_INT(3, model.selected());
  model.moveUp(); // 3 -> skip 2 -> 1
  TEST_ASSERT_EQUAL_INT(1, model.selected());
}

void test_select_ignores_disabled(void) {
  model.init(kItems, kCount);
  model.select(2); // disabled -> ignored
  TEST_ASSERT_EQUAL_INT(0, model.selected());
  model.select(3);
  TEST_ASSERT_EQUAL_INT(3, model.selected());
}

void test_open_fires_seam_with_selection(void) {
  model.init(kItems, kCount);
  model.setOpenHandler(on_open, nullptr);
  model.select(3);
  model.onOpen();
  TEST_ASSERT_TRUE(open_called);
  TEST_ASSERT_EQUAL_INT(3, opened_index);
}

// --- Widget-level (drive the real footer buttons / rows) ---

void test_footer_buttons_move_and_open(void) {
  model.init(kItems, kCount);
  model.setOpenHandler(on_open, nullptr);
  SelectableList ui = create_selectable_list(lv_screen_active(), model);

  // Up is disabled at the top; Down enabled.
  lv_obj_update_layout(lv_screen_active());
  TEST_ASSERT_TRUE(lv_obj_has_state(ui.btn_up, LV_STATE_DISABLED));
  TEST_ASSERT_FALSE(lv_obj_has_state(ui.btn_down, LV_STATE_DISABLED));

  click_center(ui.btn_down); // -> 1
  TEST_ASSERT_EQUAL_INT(1, model.selected());
  click_center(ui.btn_down); // -> 3 (skips disabled 2)
  TEST_ASSERT_EQUAL_INT(3, model.selected());
  TEST_ASSERT_TRUE(lv_obj_has_state(ui.btn_down, LV_STATE_DISABLED)); // at last enabled

  click_center(ui.btn_open);
  TEST_ASSERT_TRUE(open_called);
  TEST_ASSERT_EQUAL_INT(3, opened_index);
}

void test_row_tap_highlights(void) {
  model.init(kItems, kCount);
  SelectableList ui = create_selectable_list(lv_screen_active(), model);
  click_center(ui.rows[1]);
  TEST_ASSERT_EQUAL_INT(1, model.selected());
  // Tapping a disabled row does nothing (not clickable / ignored).
  click_center(ui.rows[2]);
  TEST_ASSERT_EQUAL_INT(1, model.selected());
}

void test_action_verb_tracks_selection(void) {
  static const SelectableListItem items[] = {
      {"Open me", nullptr, true, "Open"},
      {"Edit me", "5", true, "Edit"},
      {"Default verb", nullptr, true}, // no verb -> "Open"
  };
  model.init(items, 3);
  SelectableList ui = create_selectable_list(lv_screen_active(), model);
  lv_obj_t *label = lv_obj_get_child(ui.btn_open, 0);
  TEST_ASSERT_EQUAL_STRING("Open", lv_label_get_text(label));
  model.moveDown();
  lv_test_wait(20);
  TEST_ASSERT_EQUAL_STRING("Edit", lv_label_get_text(label));
  model.moveDown();
  lv_test_wait(20);
  TEST_ASSERT_EQUAL_STRING("Open", lv_label_get_text(label)); // null verb falls back to "Open"
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_init_selects_first_enabled);
  RUN_TEST(test_init_skips_leading_disabled);
  RUN_TEST(test_move_skips_disabled_and_saturates);
  RUN_TEST(test_select_ignores_disabled);
  RUN_TEST(test_open_fires_seam_with_selection);
  RUN_TEST(test_footer_buttons_move_and_open);
  RUN_TEST(test_row_tap_highlights);
  RUN_TEST(test_action_verb_tracks_selection);
  return UNITY_END();
}
