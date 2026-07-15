// native_ui_cyd suite — LVGL 9.5 headless test of the value-stepper editor (§24, C2). LVGL runs
// on the host with LV_USE_TEST=1; an in-memory dummy display + simulated pointer drive the real
// create_value_stepper() widgets. No board, no pixels. LovyanGFX is not linked.
//
// The pure clamp/step maths lives in NumericFieldConfig and is covered in native_logic_cyd
// (test_numeric_field); here we assert the *wiring*: −/+ move the bound subject, disable at the
// limits, the value label + caution track the subject, and the footer/value-tap seams fire.
#include <unity.h>
#include <lvgl.h>
#include "src/debugging/test/lv_test.h" // lv_test_display_create / mouse / wait (gated by LV_USE_TEST)

#include "panel.h"
#include "value_stepper.h"

// File-static view model: its lv_subject_t must outlive lv_deinit() (the widgets unlink their
// observers from it during teardown), so it lives in static storage like the shared subjects do,
// re-init()'d per test. init() also clears seams, so tests don't bleed handlers into each other.
static ValueStepperViewModel vm;

// Seam probes.
static bool commit_called;
static int32_t committed_value;
static bool cancel_called;
static bool value_tap_called;

static void on_commit(int32_t value, void *) {
  commit_called = true;
  committed_value = value;
}
static void on_cancel(void *) {
  cancel_called = true;
}
static void on_value_tap(void *) {
  value_tap_called = true;
}

// Idle timeout: 1–10 min, step 1, default 2 — a real stepper field (§24).
static NumericFieldConfig timeout_cfg() {
  return NumericFieldConfig{1, 10, 1, 2, "min", nullptr};
}

void setUp(void) {
  lv_init();
  lv_test_display_create(panel::W, panel::H);
  lv_test_indev_create_all();
  commit_called = cancel_called = value_tap_called = false;
  committed_value = 0;
}

void tearDown(void) {
  lv_deinit();
}

static void click_center(lv_obj_t *obj) {
  lv_obj_update_layout(lv_screen_active());
  lv_area_t coords;
  lv_obj_get_coords(obj, &coords);
  lv_test_mouse_click_at((coords.x1 + coords.x2) / 2, (coords.y1 + coords.y2) / 2);
  lv_test_wait(50);
}

void test_plus_and_minus_step_the_value(void) {
  vm.init(timeout_cfg(), 5);
  ValueStepper ui = create_value_stepper(lv_screen_active(), vm, "Idle timeout");

  click_center(ui.btn_plus);
  TEST_ASSERT_EQUAL_INT32(6, vm.value());
  click_center(ui.btn_minus);
  click_center(ui.btn_minus);
  TEST_ASSERT_EQUAL_INT32(4, vm.value());
}

void test_buttons_disable_at_limits(void) {
  vm.init(timeout_cfg(), 2);
  ValueStepper ui = create_value_stepper(lv_screen_active(), vm, "Idle timeout");

  // Drive to the max; + disables, − stays live.
  lv_subject_set_int(vm.valueSubject(), 10);
  TEST_ASSERT_TRUE(lv_obj_has_state(ui.btn_plus, LV_STATE_DISABLED));
  TEST_ASSERT_FALSE(lv_obj_has_state(ui.btn_minus, LV_STATE_DISABLED));
  // A tap on the disabled + is ignored (value holds at max).
  click_center(ui.btn_plus);
  TEST_ASSERT_EQUAL_INT32(10, vm.value());

  // Drive to the min; − disables, + re-enables.
  lv_subject_set_int(vm.valueSubject(), 1);
  TEST_ASSERT_TRUE(lv_obj_has_state(ui.btn_minus, LV_STATE_DISABLED));
  TEST_ASSERT_FALSE(lv_obj_has_state(ui.btn_plus, LV_STATE_DISABLED));
}

void test_value_label_tracks_subject_with_units(void) {
  vm.init(timeout_cfg(), 2);
  ValueStepper ui = create_value_stepper(lv_screen_active(), vm, "Idle timeout");

  TEST_ASSERT_EQUAL_STRING("2 min", lv_label_get_text(ui.value_label));
  lv_subject_set_int(vm.valueSubject(), 7);
  TEST_ASSERT_EQUAL_STRING("7 min", lv_label_get_text(ui.value_label));
}

void test_caution_shows_only_above_default(void) {
  vm.init(NumericFieldConfig{1, 10, 1, 2, "min", "Above default"}, 2);
  ValueStepper ui = create_value_stepper(lv_screen_active(), vm, "Idle timeout");

  // At default: hidden.
  TEST_ASSERT_TRUE(lv_obj_has_flag(ui.caution_label, LV_OBJ_FLAG_HIDDEN));
  // Above default: shown, with the configured string.
  lv_subject_set_int(vm.valueSubject(), 5);
  TEST_ASSERT_FALSE(lv_obj_has_flag(ui.caution_label, LV_OBJ_FLAG_HIDDEN));
  TEST_ASSERT_EQUAL_STRING("Above default", lv_label_get_text(ui.caution_label));
  // Back to default: hidden again.
  lv_subject_set_int(vm.valueSubject(), 2);
  TEST_ASSERT_TRUE(lv_obj_has_flag(ui.caution_label, LV_OBJ_FLAG_HIDDEN));
}

void test_save_commits_current_value(void) {
  vm.init(timeout_cfg(), 3);
  vm.setCommitHandler(on_commit, nullptr);
  ValueStepper ui = create_value_stepper(lv_screen_active(), vm, "Idle timeout");

  click_center(ui.btn_plus); // 3 -> 4
  click_center(ui.btn_save);
  TEST_ASSERT_TRUE(commit_called);
  TEST_ASSERT_EQUAL_INT32(4, committed_value);
}

void test_cancel_restores_initial_and_fires_seam(void) {
  vm.init(timeout_cfg(), 3);
  vm.setCancelHandler(on_cancel, nullptr);
  ValueStepper ui = create_value_stepper(lv_screen_active(), vm, "Idle timeout");

  click_center(ui.btn_plus);
  click_center(ui.btn_plus); // 3 -> 5
  TEST_ASSERT_EQUAL_INT32(5, vm.value());
  click_center(ui.btn_cancel);
  TEST_ASSERT_TRUE(cancel_called);
  TEST_ASSERT_EQUAL_INT32(3, vm.value()); // restored to the opening value
}

void test_tapping_value_opens_keypad_seam(void) {
  vm.init(timeout_cfg(), 2);
  vm.setValueTapHandler(on_value_tap, nullptr);
  ValueStepper ui = create_value_stepper(lv_screen_active(), vm, "Idle timeout");

  click_center(ui.value_label);
  TEST_ASSERT_TRUE(value_tap_called);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_plus_and_minus_step_the_value);
  RUN_TEST(test_buttons_disable_at_limits);
  RUN_TEST(test_value_label_tracks_subject_with_units);
  RUN_TEST(test_caution_shows_only_above_default);
  RUN_TEST(test_save_commits_current_value);
  RUN_TEST(test_cancel_restores_initial_and_fires_seam);
  RUN_TEST(test_tapping_value_opens_keypad_seam);
  return UNITY_END();
}
