// native_ui_cyd suite — LVGL 9.5 headless test of the Settings hub + panels (§24, C8 slice). The
// store logic is covered in native_logic_cyd (test_settings_store); here we assert the *wiring*:
// every actionable panel is a selectable list, hub Open navigates to each panel, Back returns, the
// Advanced master toggle and the auto-brightness/units rows flip in place + persist, and a cap/idle
// edit through the shared editors commits back into the store and republishes the cross-screen
// subjects.
#include <unity.h>
#include <lvgl.h>
#include "src/debugging/test/lv_test.h"

#include "helpers/fake_settings_storage.h"
#include "settings_screen.h"
#include "settings_store.h"
#include "subjects.h"

// Documented hub row order (settings_screen.cpp HubIndex). Enabled: 0,1,2,6,7.
enum { ROW_DISPLAY = 0, ROW_TEMP = 1, ROW_SLEEP = 2, ROW_ABOUT = 6, ROW_ADVANCED = 7 };

static FakeSettingsStorage fs;
static SettingsStore store(fs);
static SettingsScreen screen;

static bool exited;
static void on_exit(void *) {
  exited = true;
}

void setUp(void) {
  lv_init();
  lv_test_display_create(320, 240);
  lv_test_indev_create_all();
  ui_subjects_init();
  fs.present = false;
  fs.blob.clear();
  fs.saveCalls = 0;
  store.load(); // defaults
  exited = false;
}

void tearDown(void) {
  lv_deinit();
}

// Act on a row through the real list model (select + Open fires the panel's open handler).
static void open_row(int index) {
  screen.listModel().select(index);
  screen.listModel().onOpen();
}

void test_begin_shows_hub(void) {
  screen.begin(lv_screen_active(), store);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsPage::Hub), static_cast<int>(screen.page()));
}

void test_open_categories_and_back(void) {
  screen.begin(lv_screen_active(), store);
  open_row(ROW_DISPLAY);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsPage::DisplayUnits),
                        static_cast<int>(screen.page()));
  screen.back();
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsPage::Hub), static_cast<int>(screen.page()));

  open_row(ROW_TEMP);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsPage::TempLimits),
                        static_cast<int>(screen.page()));
  screen.back();

  open_row(ROW_SLEEP);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsPage::SleepWake), static_cast<int>(screen.page()));
  screen.back();

  open_row(ROW_ABOUT);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsPage::About), static_cast<int>(screen.page()));
}

void test_back_from_hub_calls_exit(void) {
  screen.setExitHandler(on_exit, nullptr);
  screen.begin(lv_screen_active(), store);
  screen.back();
  TEST_ASSERT_TRUE(exited);
}

void test_advanced_master_toggle(void) {
  screen.begin(lv_screen_active(), store);
  TEST_ASSERT_FALSE(store.advancedUnlocked());
  open_row(ROW_ADVANCED);
  TEST_ASSERT_TRUE(store.advancedUnlocked());
  TEST_ASSERT_EQUAL_INT(1, lv_subject_get_int(&subj_advanced));
  TEST_ASSERT_TRUE(fs.saveCalls > 0); // persisted
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsPage::Hub), static_cast<int>(screen.page()));
  open_row(ROW_ADVANCED);
  TEST_ASSERT_FALSE(store.advancedUnlocked()); // toggles back
}

void test_units_cycle_persists_and_publishes(void) {
  screen.begin(lv_screen_active(), store);
  open_row(ROW_DISPLAY);
  // Display & units rows: [0] units (enum, Open cycles), [1] auto-brightness, [2] bias.
  open_row(0);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(TempUnits::Fahrenheit), static_cast<int>(store.units()));
  TEST_ASSERT_EQUAL_INT(1, lv_subject_get_int(&subj_units)); // Home reads this
  TEST_ASSERT_TRUE(fs.saveCalls > 0);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsPage::DisplayUnits),
                        static_cast<int>(screen.page())); // stays on the panel
  open_row(0);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(TempUnits::Celsius), static_cast<int>(store.units()));
}

void test_auto_brightness_toggle_persists(void) {
  screen.begin(lv_screen_active(), store);
  open_row(ROW_DISPLAY);
  TEST_ASSERT_TRUE(store.autoBrightness()); // default on
  open_row(1);
  TEST_ASSERT_FALSE(store.autoBrightness());
  TEST_ASSERT_TRUE(fs.saveCalls > 0);
}

void test_uv_cap_edit_commits_and_publishes(void) {
  screen.begin(lv_screen_active(), store);
  open_row(ROW_TEMP);
  open_row(0); // UV cure max -> keypad editor
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsPage::Editor), static_cast<int>(screen.page()));

  screen.keypadVm().onClear();
  screen.keypadVm().onDigit(1);
  screen.keypadVm().onDigit(1);
  screen.keypadVm().onDigit(5);
  screen.keypadVm().onOk();

  TEST_ASSERT_EQUAL_INT32(115, store.uvMaxCap());
  TEST_ASSERT_EQUAL_INT(115, lv_subject_get_int(&subj_uv_cap));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsPage::TempLimits),
                        static_cast<int>(screen.page())); // returned to the panel
}

void test_idle_timeout_edit_uses_stepper(void) {
  screen.begin(lv_screen_active(), store);
  open_row(ROW_SLEEP);
  open_row(0); // Idle timeout -> stepper editor
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsPage::Editor), static_cast<int>(screen.page()));
  screen.stepperVm().onPlus(); // 2 -> 3
  screen.stepperVm().onSave();
  TEST_ASSERT_EQUAL_INT32(3, store.idleTimeoutMin());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsPage::SleepWake), static_cast<int>(screen.page()));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_begin_shows_hub);
  RUN_TEST(test_open_categories_and_back);
  RUN_TEST(test_back_from_hub_calls_exit);
  RUN_TEST(test_advanced_master_toggle);
  RUN_TEST(test_units_cycle_persists_and_publishes);
  RUN_TEST(test_auto_brightness_toggle_persists);
  RUN_TEST(test_uv_cap_edit_commits_and_publishes);
  RUN_TEST(test_idle_timeout_edit_uses_stepper);
  return UNITY_END();
}
