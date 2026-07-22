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

#include "helpers/fake_clock.h"
#include "helpers/fake_profile_storage.h"
#include "helpers/fake_settings_storage.h"
#include "helpers/pipe_transport.h"
#include "management_client.h"
#include "management_responder.h"
#include "panel.h"
#include "settings_screen.h"
#include "settings_store.h"
#include "stock_seed.h"
#include "subjects.h"

// Documented hub row order (settings_screen.cpp HubIndex). Enabled: 0,1,4,5,6.
enum { ROW_DISPLAY = 0, ROW_TEMP = 1, ROW_PROFILES = 4, ROW_ABOUT = 5, ROW_ADVANCED = 6 };

static FakeSettingsStorage fs;
static SettingsStore store(fs);
static SettingsScreen screen;

// --- the remote stack, for §24's Restore stock profiles (mirrors test_profile_library's rig) ---
// A real ManagementResponder over real controller stores, so the restore is exercised end-to-end
// rather than against a stub that could agree with a wrong client.
static LoopbackPipe pipe;
static FakeClock clk;
static FakeProfileStorage cure_fs;
static FakeProfileStorage reflow_fs;
static control::ProfileStore cure_store(cure_fs, oven_Mode_MODE_CURE);
static control::ProfileStore reflow_store(reflow_fs, oven_Mode_MODE_REFLOW);
static protocol::MessageRouter ctrl_router;
static protocol::FrameLink ctrl_link(pipe.b(), TF_SLAVE, ctrl_router);
static ManagementResponder responder(ctrl_link, cure_store, reflow_store);
static protocol::MessageRouter cyd_router;
static protocol::FrameLink cyd_link(pipe.a(), TF_MASTER, cyd_router);
static ManagementClient client(cyd_link, clk);

// Pump both link directions until the screen's restore leaves Busy (or a bound is hit).
static void settle() {
  for (int i = 0; i < 12; ++i) {
    ctrl_link.poll();
    cyd_link.poll();
    client.service();
    screen.poll();
  }
}

static bool exited;
static void on_exit(void *) {
  exited = true;
}

void setUp(void) {
  lv_init();
  lv_test_display_create(panel::W, panel::H);
  lv_test_indev_create_all();
  ui_subjects_init();
  // You reach Settings from Home only with a healthy link (Home gates the tile), so the default
  // precondition is LINK_OK — otherwise the hub's now link-gated category rows are disabled and the
  // navigation tests can't open them. The gating itself is exercised by test_hub_gates_on_link.
  lv_subject_set_int(&subj_link_state, LINK_OK);
  fs.present = false;
  fs.blob.clear();
  fs.saveCalls = 0;
  store.load(); // defaults
  exited = false;
  cure_fs.entries.clear();
  reflow_fs.entries.clear();
  responder.reset(); // clear the dedup cache between tests
  ctrl_router.setObserver(responder);
  cyd_router.setObserver(client);
  client.clear();
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

// Settings live on the controller now (§7), so a dropped link greys the editable hub categories
// (they can't be selected or opened), while About — local device info — stays reachable. The hub
// rebuilds reactively when the link flips.
void test_hub_gates_on_link(void) {
  lv_subject_set_int(&subj_link_state, LINK_OK);
  screen.begin(lv_screen_active(), store);
  open_row(ROW_DISPLAY); // healthy: opens
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsPage::DisplayUnits),
                        static_cast<int>(screen.page()));
  screen.back();

  // Link drops → the observer rebuilds the hub with the editable categories disabled.
  lv_subject_set_int(&subj_link_state, LINK_NONE);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsPage::Hub), static_cast<int>(screen.page()));
  screen.listModel().select(ROW_DISPLAY); // disabled → select() rejects it
  TEST_ASSERT_NOT_EQUAL(ROW_DISPLAY, screen.listModel().selected());
  screen.listModel().select(ROW_TEMP);
  TEST_ASSERT_NOT_EQUAL(ROW_TEMP, screen.listModel().selected());
  open_row(ROW_ABOUT); // local info → still opens
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsPage::About), static_cast<int>(screen.page()));

  // Reconnect → the categories open again.
  screen.back();
  lv_subject_set_int(&subj_link_state, LINK_OK);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsPage::Hub), static_cast<int>(screen.page()));
  open_row(ROW_DISPLAY);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsPage::DisplayUnits),
                        static_cast<int>(screen.page()));
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

// --- §24 Restore stock profiles ---

// Without a client the row stays disabled and unopenable: the library lives on the controller
// (§7), so with no link to it there is nothing a restore could do. Same treatment as the
// genuinely-unbuilt "soon" rows, which is honest — this one is unreachable for a different reason
// but is equally not going to work.
void test_profiles_row_needs_a_client(void) {
  screen.begin(lv_screen_active(), store); // no client
  TEST_ASSERT_FALSE(screen.listModel().item(ROW_PROFILES).enabled);
  // Not "still Hub": the list model refuses to select a disabled row, so the highlight stays put
  // and Open fires on whatever IS selected. What matters is that the panel is unreachable.
  open_row(ROW_PROFILES);
  TEST_ASSERT_NOT_EQUAL(static_cast<int>(SettingsPage::Profiles), static_cast<int>(screen.page()));
}

// With one, the row opens a real panel offering both modes separately (§7's libraries are
// independent, so repairing cure has no business touching reflow).
void test_profiles_panel_offers_both_modes(void) {
  screen.begin(lv_screen_active(), store, &client);
  TEST_ASSERT_TRUE(screen.listModel().item(ROW_PROFILES).enabled);
  open_row(ROW_PROFILES);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsPage::Profiles), static_cast<int>(screen.page()));
  TEST_ASSERT_EQUAL_INT(2, screen.listModel().count());
  screen.back();
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsPage::Hub), static_cast<int>(screen.page()));
}

// Open must not restore anything on its own — §23's confirm step is the guard, and a one-tap
// rewrite of the library would be exactly the kind of thing that guard exists to prevent.
void test_restore_requires_the_confirm(void) {
  screen.begin(lv_screen_active(), store, &client);
  open_row(ROW_PROFILES);
  open_row(0); // "Restore cure stock" -> raises the confirm dialog, sends nothing
  settle();
  control::ProfileStore::Summary rows[control::ProfileStore::kMaxListed];
  TEST_ASSERT_EQUAL_UINT32(0, cure_store.list(rows, control::ProfileStore::kMaxListed));

  screen.cancelRestore();
  settle();
  TEST_ASSERT_EQUAL_UINT32(0, cure_store.list(rows, control::ProfileStore::kMaxListed));
}

// The whole round trip: confirm -> request -> the real responder seeds from the compiled-in table
// -> the panel reports it. Driven through the reflow store, which is the one the stock table has
// an entry for.
void test_restore_confirmed_repopulates_the_library(void) {
  screen.begin(lv_screen_active(), store, &client);
  open_row(ROW_PROFILES);
  open_row(1); // "Restore reflow stock"
  screen.confirmRestore();
  settle();

  control::ProfileStore::Summary rows[control::ProfileStore::kMaxListed];
  const size_t n = reflow_store.list(rows, control::ProfileStore::kMaxListed);
  TEST_ASSERT_TRUE(n > 0);
  oven_Profile p = oven_Profile_init_zero;
  TEST_ASSERT_TRUE(reflow_store.load(rows[0].name, p));
  TEST_ASSERT_TRUE(p.stock);
  // Still on the panel that issued it, with a verdict rather than a modal (§22 keeps that rare).
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsPage::Profiles), static_cast<int>(screen.page()));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_begin_shows_hub);
  RUN_TEST(test_hub_gates_on_link);
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
  RUN_TEST(test_profiles_row_needs_a_client);
  RUN_TEST(test_profiles_panel_offers_both_modes);
  RUN_TEST(test_restore_requires_the_confirm);
  RUN_TEST(test_restore_confirmed_repopulates_the_library);
  return UNITY_END();
}
