// native_ui_cyd suite — LVGL 9.5 headless test of the Settings hub + panels (§24, C8 slice). The
// store logic is covered in native_logic_cyd (test_settings_store); here we assert the *wiring*:
// every actionable panel is a selectable list, hub Open navigates to each panel, Back returns, the
// Advanced master toggle and the auto-brightness/units rows flip in place + persist, and a cap/idle
// edit through the shared editors commits back into the store and republishes the cross-screen
// subjects.
#include <cstring>
#include <unity.h>
#include <lvgl.h>
#include "src/debugging/test/lv_test.h"

#include "helpers/fake_settings_storage.h"
#include "panel.h"
#include "settings_screen.h"
#include "settings_store.h"
#include "subjects.h"

// Documented hub row order (settings_screen.cpp HubIndex). Enabled: 0,1,5,6.
enum { ROW_DISPLAY = 0, ROW_TEMP = 1, ROW_ABOUT = 5, ROW_ADVANCED = 6 };

static FakeSettingsStorage fs;
static SettingsStore store(fs);
static SettingsScreen screen;

static bool exited;
static void on_exit(void *) {
  exited = true;
}

void setUp(void) {
  lv_init();
  lv_test_display_create(panel::W, panel::H);
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

// A board with no light sensor (the 3.5" panel) publishes subj_has_ambient_light = 0, and the
// auto-brightness row is then not built at all — a settings list lists things you can change, and
// a row that can never change on this hardware is furniture. (It was briefly disabled-but-visible
// showing "Not fitted"; that lost the argument.) The capability reaches this screen as data —
// there is deliberately no board flag to test against here.
void test_auto_brightness_row_absent_without_sensor(void) {
  lv_subject_set_int(&subj_has_ambient_light, 0);
  screen.begin(lv_screen_active(), store);
  open_row(ROW_DISPLAY);
  const bool before = store.autoBrightness();

  // Three rows: units, the absolute brightness, idle timeout. No auto row under any index.
  TEST_ASSERT_EQUAL_INT(3, screen.listModel().count());
  for (int i = 0; i < screen.listModel().count(); i++) {
    TEST_ASSERT_NOT_EQUAL_INT(0, strcmp(screen.listModel().item(i).label, "Auto-brightness"));
  }

  // The stored preference is left alone rather than clamped: it is a user preference, and it is
  // meaningful again the moment this firmware runs on a board that has a sensor.
  TEST_ASSERT_EQUAL(before, store.autoBrightness());
  TEST_ASSERT_EQUAL_INT(0, fs.saveCalls);
}

// The other half of the contract: with a sensor present (the default), the row IS built and
// selectable. Without this, the test above would still pass if the row had simply been deleted
// for every board.
void test_auto_brightness_row_present_with_sensor(void) {
  lv_subject_set_int(&subj_has_ambient_light, 1);
  screen.begin(lv_screen_active(), store);
  open_row(ROW_DISPLAY);
  TEST_ASSERT_EQUAL_INT(4, screen.listModel().count());
  TEST_ASSERT_EQUAL_STRING("Auto-brightness", screen.listModel().item(1).label);
  screen.listModel().select(1);
  TEST_ASSERT_EQUAL_INT(1, screen.listModel().selected());
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
  lv_subject_set_int(&subj_has_ambient_light, 1); // with a sensor: units, auto, bias, idle
  screen.begin(lv_screen_active(), store);
  open_row(ROW_DISPLAY);
  TEST_ASSERT_EQUAL_STRING("Idle timeout", screen.listModel().item(3).label);
  open_row(3); // Idle timeout -> stepper editor
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsPage::Editor), static_cast<int>(screen.page()));
  screen.stepperVm().onPlus(); // 2 -> 3
  screen.stepperVm().onSave();
  TEST_ASSERT_EQUAL_INT32(3, store.idleTimeoutMin());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsPage::DisplayUnits),
                        static_cast<int>(screen.page())); // back to the panel it lives on
}

// A board with no light sensor swaps the bias row for a plain absolute brightness control: a bias
// is a trim on an ambient reading, and with no reading it would be a trim on a constant. The
// capability arrives as DATA (subj_has_ambient_light) — there is deliberately no board flag for
// this screen to test against.
void test_no_sensor_swaps_bias_for_absolute_brightness(void) {
  lv_subject_set_int(&subj_has_ambient_light, 0);
  screen.begin(lv_screen_active(), store);
  open_row(ROW_DISPLAY);

  // With the auto row gone, the brightness is row 1 — and Open must still route by what was
  // BUILT, not by a hard-coded index (that mapping is exactly what DisplayRow exists to keep).
  TEST_ASSERT_EQUAL_STRING("Screen brightness", screen.listModel().item(1).label);
  open_row(1);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsPage::Editor), static_cast<int>(screen.page()));
  TEST_ASSERT_TRUE(screen.isEditingScreenBrightness());
  TEST_ASSERT_FALSE(screen.isEditingBrightnessBias());

  const int32_t before_bias = store.brightnessBias();
  screen.stepperVm().onMinus();                               // 100 -> 90
  TEST_ASSERT_EQUAL_INT32(90, screen.liveScreenBrightness()); // live preview, pre-commit
  screen.stepperVm().onSave();
  TEST_ASSERT_EQUAL_INT32(90, store.screenBrightnessPct());
  TEST_ASSERT_EQUAL_INT32(before_bias, store.brightnessBias()); // the bias is left alone
  TEST_ASSERT_TRUE(fs.saveCalls > 0);                           // persisted -> survives a restart
}

// The other half: with a sensor the row is still the bias, so the test above cannot pass by the
// screen having simply dropped the bias field.
void test_sensor_keeps_the_bias_row(void) {
  lv_subject_set_int(&subj_has_ambient_light, 1);
  screen.begin(lv_screen_active(), store);
  open_row(ROW_DISPLAY);
  TEST_ASSERT_EQUAL_STRING("Brightness bias", screen.listModel().item(2).label);
  open_row(2);
  TEST_ASSERT_TRUE(screen.isEditingBrightnessBias());
  TEST_ASSERT_FALSE(screen.isEditingScreenBrightness());
}

// There is no Sleep & wake panel. The never-sleep-during-a-run and stay-awake-while-HOT rules are
// fixed policy (§17), not settings, so they are not rendered; that left one real row, and a menu
// level holding one row charges a tap for nothing — Idle timeout moved in with the other
// "what the screen does when you are not touching it" settings.
void test_no_sleep_panel_and_idle_timeout_lives_on_display(void) {
  lv_subject_set_int(&subj_has_ambient_light, 1);
  screen.begin(lv_screen_active(), store);
  for (int i = 0; i < screen.listModel().count(); i++) {
    TEST_ASSERT_NOT_EQUAL_INT(0, strcmp(screen.listModel().item(i).label, "Sleep & wake"));
  }
  open_row(ROW_DISPLAY);
  TEST_ASSERT_EQUAL_INT(4, screen.listModel().count()); // units, auto, bias, idle
  TEST_ASSERT_EQUAL_STRING("Idle timeout", screen.listModel().item(3).label);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_begin_shows_hub);
  RUN_TEST(test_open_categories_and_back);
  RUN_TEST(test_back_from_hub_calls_exit);
  RUN_TEST(test_advanced_master_toggle);
  RUN_TEST(test_auto_brightness_row_absent_without_sensor);
  RUN_TEST(test_auto_brightness_row_present_with_sensor);
  RUN_TEST(test_no_sensor_swaps_bias_for_absolute_brightness);
  RUN_TEST(test_sensor_keeps_the_bias_row);
  RUN_TEST(test_units_cycle_persists_and_publishes);
  RUN_TEST(test_auto_brightness_toggle_persists);
  RUN_TEST(test_uv_cap_edit_commits_and_publishes);
  RUN_TEST(test_idle_timeout_edit_uses_stepper);
  RUN_TEST(test_no_sleep_panel_and_idle_timeout_lives_on_display);
  return UNITY_END();
}
