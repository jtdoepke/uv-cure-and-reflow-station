// native_ui_cyd suite — LVGL 9.5 headless test of the on-screen numeric keypad (§26, C1). LVGL
// runs on the host with LV_USE_TEST=1; an in-memory dummy display + simulated pointer drive the
// real create_numeric_keypad() widgets. No board, no pixels. LovyanGFX is not linked.
//
// The pure digit/clamp maths lives in NumericEntry and is covered in native_logic_cyd
// (test_numeric_entry); here we assert the *wiring*: digit keys build the bound value, ⌫ deletes
// and long-press empties, an over-max digit is blocked, OK disables out of range, the readout
// goes amber while invalid, and the OK/Cancel seams fire.
#include <unity.h>
#include <lvgl.h>
#include "src/debugging/test/lv_test.h" // lv_test_display_create / mouse / wait (gated by LV_USE_TEST)

#include "numeric_keypad.h"
#include "panel.h"
#include "theme.h"

// File-static view model: its lv_subject_t must outlive lv_deinit() (the widgets unlink their
// observers from it during teardown), so it lives in static storage like the shared subjects do,
// re-init()'d per test. init() also clears seams, so tests don't bleed handlers into each other.
static NumericKeypadViewModel vm;

// Seam probes.
static bool commit_called;
static int32_t committed_value;
static bool cancel_called;

static void on_commit(int32_t value, void *) {
  commit_called = true;
  committed_value = value;
}
static void on_cancel(void *) {
  cancel_called = true;
}

// UV temp cap: 60–250 °C, default 100 — a real keypad field (fails the >20-step rule, §24/§26).
static NumericFieldConfig cap_cfg() {
  return NumericFieldConfig{60, 250, 1, 100, "°C", nullptr};
}

void setUp(void) {
  lv_init();
  lv_test_display_create(panel::W, panel::H);
  lv_test_indev_create_all();
  commit_called = cancel_called = false;
  committed_value = 0;
}

void tearDown(void) {
  lv_deinit();
}

static void center_of(lv_obj_t *obj, int32_t *x, int32_t *y) {
  lv_obj_update_layout(lv_screen_active());
  lv_area_t coords;
  lv_obj_get_coords(obj, &coords);
  *x = (coords.x1 + coords.x2) / 2;
  *y = (coords.y1 + coords.y2) / 2;
}

static void click(lv_obj_t *obj) {
  int32_t x, y;
  center_of(obj, &x, &y);
  lv_test_mouse_click_at(x, y);
  lv_test_wait(50);
}

// Press-and-hold past the long-press threshold, then release (drives LV_EVENT_LONG_PRESSED).
static void long_press(lv_obj_t *obj) {
  int32_t x, y;
  center_of(obj, &x, &y);
  lv_test_mouse_move_to(x, y);
  lv_test_mouse_press();
  lv_test_wait(600); // exceed LVGL's default long-press time (400 ms)
  lv_test_mouse_release();
  lv_test_wait(50);
}

// Type a run of digits via their keys.
static void type_digits(NumericKeypad &ui, const char *digits) {
  for (const char *p = digits; *p != '\0'; ++p) {
    click(ui.keys[*p - '0']);
  }
}

void test_digit_keys_build_the_value(void) {
  vm.init(cap_cfg(), 100);
  NumericKeypad ui = create_numeric_keypad(lv_screen_active(), vm, "Target temp");

  long_press(ui.btn_backspace); // empty the preloaded 100 first
  type_digits(ui, "245");
  TEST_ASSERT_EQUAL_INT32(245, vm.value());
  TEST_ASSERT_EQUAL_STRING("245 \xC2\xB0"
                           "C",
                           lv_label_get_text(ui.value_label));
}

void test_backspace_deletes_last_digit(void) {
  vm.init(cap_cfg(), 100);
  NumericKeypad ui = create_numeric_keypad(lv_screen_active(), vm, "Target temp");

  long_press(ui.btn_backspace);
  type_digits(ui, "245");
  click(ui.btn_backspace);
  TEST_ASSERT_EQUAL_INT32(24, vm.value());
}

void test_long_press_backspace_empties(void) {
  vm.init(cap_cfg(), 100);
  NumericKeypad ui = create_numeric_keypad(lv_screen_active(), vm, "Target temp");

  long_press(ui.btn_backspace);
  TEST_ASSERT_TRUE(vm.entry().isEmpty());
  TEST_ASSERT_EQUAL_STRING("", lv_label_get_text(ui.value_label)); // empty renders blank
}

void test_over_max_digit_is_blocked(void) {
  vm.init(cap_cfg(), 100);
  NumericKeypad ui = create_numeric_keypad(lv_screen_active(), vm, "Target temp");

  long_press(ui.btn_backspace);
  type_digits(ui, "25");
  click(ui.keys[5]); // 25*10+5 = 255 > 250 → refused
  TEST_ASSERT_EQUAL_INT32(25, vm.value());
  click(ui.keys[0]); // 250 is in range → accepted
  TEST_ASSERT_EQUAL_INT32(250, vm.value());
}

void test_ok_disables_until_in_range(void) {
  vm.init(cap_cfg(), 100);
  NumericKeypad ui = create_numeric_keypad(lv_screen_active(), vm, "Target temp");

  long_press(ui.btn_backspace);
  TEST_ASSERT_TRUE(lv_obj_has_state(ui.btn_ok, LV_STATE_DISABLED)); // empty
  click(ui.keys[6]);                                                // 6 < min 60
  TEST_ASSERT_TRUE(lv_obj_has_state(ui.btn_ok, LV_STATE_DISABLED));
  click(ui.keys[0]); // 60 == min
  TEST_ASSERT_FALSE(lv_obj_has_state(ui.btn_ok, LV_STATE_DISABLED));
}

void test_value_goes_amber_while_invalid(void) {
  vm.init(cap_cfg(), 100);
  NumericKeypad ui = create_numeric_keypad(lv_screen_active(), vm, "Target temp");

  long_press(ui.btn_backspace); // empty → invalid
  TEST_ASSERT_TRUE(lv_color_eq(lv_obj_get_style_text_color(ui.value_label, LV_PART_MAIN),
                               theme::col(theme::WARN)));
  type_digits(ui, "60"); // in range → normal text colour
  TEST_ASSERT_TRUE(lv_color_eq(lv_obj_get_style_text_color(ui.value_label, LV_PART_MAIN),
                               theme::col(theme::TEXT)));
}

void test_ok_commits_the_value(void) {
  vm.init(cap_cfg(), 100);
  vm.setCommitHandler(on_commit, nullptr);
  NumericKeypad ui = create_numeric_keypad(lv_screen_active(), vm, "Target temp");

  long_press(ui.btn_backspace);
  type_digits(ui, "245");
  click(ui.btn_ok);
  TEST_ASSERT_TRUE(commit_called);
  TEST_ASSERT_EQUAL_INT32(245, committed_value);
}

void test_ok_out_of_range_does_not_commit(void) {
  vm.init(cap_cfg(), 100);
  vm.setCommitHandler(on_commit, nullptr);
  NumericKeypad ui = create_numeric_keypad(lv_screen_active(), vm, "Target temp");

  long_press(ui.btn_backspace);
  type_digits(ui, "25"); // 25 < min 60 → OK disabled, seam guarded
  click(ui.btn_ok);
  TEST_ASSERT_FALSE(commit_called);
}

void test_cancel_fires_seam(void) {
  vm.init(cap_cfg(), 100);
  vm.setCancelHandler(on_cancel, nullptr);
  NumericKeypad ui = create_numeric_keypad(lv_screen_active(), vm, "Target temp");

  click(ui.btn_cancel);
  TEST_ASSERT_TRUE(cancel_called);
}

void test_caution_shows_only_above_default(void) {
  vm.init(NumericFieldConfig{60, 250, 1, 100, "°C", "Above default"}, 100);
  NumericKeypad ui = create_numeric_keypad(lv_screen_active(), vm, "UV cure max");

  // At default (100): hidden.
  TEST_ASSERT_TRUE(lv_obj_has_flag(ui.caution_label, LV_OBJ_FLAG_HIDDEN));
  // Type 150 (> default) → shown with the configured string.
  long_press(ui.btn_backspace);
  type_digits(ui, "150");
  TEST_ASSERT_FALSE(lv_obj_has_flag(ui.caution_label, LV_OBJ_FLAG_HIDDEN));
  TEST_ASSERT_EQUAL_STRING("Above default", lv_label_get_text(ui.caution_label));
  // Back to default (100) → hidden again.
  long_press(ui.btn_backspace);
  type_digits(ui, "100");
  TEST_ASSERT_TRUE(lv_obj_has_flag(ui.caution_label, LV_OBJ_FLAG_HIDDEN));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_digit_keys_build_the_value);
  RUN_TEST(test_backspace_deletes_last_digit);
  RUN_TEST(test_long_press_backspace_empties);
  RUN_TEST(test_over_max_digit_is_blocked);
  RUN_TEST(test_ok_disables_until_in_range);
  RUN_TEST(test_value_goes_amber_while_invalid);
  RUN_TEST(test_ok_commits_the_value);
  RUN_TEST(test_ok_out_of_range_does_not_commit);
  RUN_TEST(test_cancel_fires_seam);
  RUN_TEST(test_caution_shows_only_above_default);
  return UNITY_END();
}
