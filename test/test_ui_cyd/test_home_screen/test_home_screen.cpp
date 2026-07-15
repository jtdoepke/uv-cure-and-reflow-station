// native_ui_cyd suite — LVGL 9.5 headless UI test. LVGL runs on the host with LV_USE_TEST=1;
// an in-memory dummy display + simulated pointer drive the real create_home_screen() widgets.
// No board, no pixels on glass. LovyanGFX is not linked (lib_ignore in the env).
//
// Two layers of assertion: the view model's pure state→view mappers directly, and the rendered
// screen's behaviour (tile taps publish nav intents; link health gates the mode tiles; state
// changes update the band).
#include <unity.h>
#include <lvgl.h>
#include "src/debugging/test/lv_test.h" // lv_test_display_create / mouse / wait (gated by LV_USE_TEST)

#include "home_screen.h"
#include "panel.h"
#include "home_viewmodel.h"
#include "subjects.h"
#include "theme.h"

void setUp(void) {
  lv_init();
  lv_test_display_create(panel::W, panel::H);
  lv_test_indev_create_all();
  ui_subjects_init(); // subjects must exist before the screen binds to them
}

void tearDown(void) {
  lv_deinit(); // fresh LVGL per test so static widget state doesn't leak between runs
}

static void click_center(lv_obj_t *obj) {
  lv_obj_update_layout(lv_screen_active());
  lv_area_t coords;
  lv_obj_get_coords(obj, &coords);
  lv_test_mouse_click_at((coords.x1 + coords.x2) / 2, (coords.y1 + coords.y2) / 2);
  lv_test_wait(50);
}

// --- View-model mappers (pure logic) ---

void test_vm_state_pairs_word_and_colour(void) {
  TEST_ASSERT_EQUAL_STRING("IDLE", HomeViewModel::stateText(RUN_IDLE));
  TEST_ASSERT_EQUAL_STRING("HOT", HomeViewModel::stateText(RUN_HOT));
  TEST_ASSERT_EQUAL_UINT32(theme::IDLE, HomeViewModel::stateColor(RUN_IDLE));
  TEST_ASSERT_EQUAL_UINT32(theme::WARN, HomeViewModel::stateColor(RUN_HOT));
  TEST_ASSERT_EQUAL_UINT32(theme::FAULT, HomeViewModel::stateColor(RUN_FAULT));
}

void test_vm_link_gates_mode_and_pairs_word(void) {
  TEST_ASSERT_TRUE(HomeViewModel::modeEnabled(LINK_OK));
  TEST_ASSERT_FALSE(HomeViewModel::modeEnabled(LINK_NONE));
  TEST_ASSERT_FALSE(HomeViewModel::modeEnabled(LINK_SCHEMA));
  // Every link state carries a word, and only OK is green.
  TEST_ASSERT_NOT_NULL(HomeViewModel::linkText(LINK_NONE));
  TEST_ASSERT_EQUAL_UINT32(theme::IDLE, HomeViewModel::linkColor(LINK_OK));
  TEST_ASSERT_EQUAL_UINT32(theme::FAULT, HomeViewModel::linkColor(LINK_NONE));
}

// The §9 link maps onto the indicator: nothing there is "no link"; present but disagreeing on
// the .proto is a schema fault, not a healthy link (fail-closed — those two must never collapse
// into one state, since only one of them is fixed by plugging a cable in).
void test_vm_link_state_from_handshake(void) {
  TEST_ASSERT_EQUAL_INT(LINK_NONE, HomeViewModel::linkStateFrom(false, false, false));
  TEST_ASSERT_EQUAL_INT(LINK_OK, HomeViewModel::linkStateFrom(true, true, true));
  TEST_ASSERT_EQUAL_INT(LINK_SCHEMA, HomeViewModel::linkStateFrom(true, false, true));
  // Telemetry flowing before the handshake lands (a real, brief window at boot) is not yet a
  // healthy link: never report OK to something we have not actually handshaken with.
  TEST_ASSERT_EQUAL_INT(LINK_NONE, HomeViewModel::linkStateFrom(false, true, true));
}

// The regression this exists for: the handshake LATCHES, so a controller that is unplugged
// still reads saw_peer=matched=true forever. Only liveness decays, and it must win — otherwise
// Home cheerfully claims "Link" over a cable lying on the bench, which is exactly what it did.
void test_vm_dead_link_beats_a_latched_handshake(void) {
  TEST_ASSERT_EQUAL_INT(LINK_NONE, HomeViewModel::linkStateFrom(true, true, false));
  TEST_ASSERT_EQUAL_INT(LINK_NONE, HomeViewModel::linkStateFrom(true, false, false));
}

// --- Rendered-screen behaviour ---

void test_mode_tiles_publish_nav_intent(void) {
  lv_subject_set_int(&subj_link_state, LINK_OK); // tiles must be enabled to receive the tap
  HomeScreen ui = create_home_screen(lv_screen_active());

  click_center(ui.btn_cure);
  TEST_ASSERT_EQUAL_INT(NAV_CURE_SETUP, lv_subject_get_int(&subj_nav_request));

  click_center(ui.btn_reflow);
  TEST_ASSERT_EQUAL_INT(NAV_REFLOW_SETUP, lv_subject_get_int(&subj_nav_request));
}

void test_secondary_row_publishes_nav_intent(void) {
  HomeScreen ui = create_home_screen(lv_screen_active());

  click_center(ui.btn_profiles);
  TEST_ASSERT_EQUAL_INT(NAV_PROFILES, lv_subject_get_int(&subj_nav_request));
  click_center(ui.btn_calibrate);
  TEST_ASSERT_EQUAL_INT(NAV_CALIBRATE, lv_subject_get_int(&subj_nav_request));
  click_center(ui.btn_settings);
  TEST_ASSERT_EQUAL_INT(NAV_SETTINGS, lv_subject_get_int(&subj_nav_request));
}

void test_link_loss_disables_mode_tiles(void) {
  HomeScreen ui = create_home_screen(lv_screen_active());

  // Healthy link: tiles clickable.
  lv_subject_set_int(&subj_link_state, LINK_OK);
  TEST_ASSERT_TRUE(lv_obj_has_flag(ui.btn_cure, LV_OBJ_FLAG_CLICKABLE));
  TEST_ASSERT_FALSE(lv_obj_has_state(ui.btn_cure, LV_STATE_DISABLED));

  // Link lost: tiles gated (not clickable + shown disabled), and a tap is ignored.
  lv_subject_set_int(&subj_nav_request, NAV_NONE);
  lv_subject_set_int(&subj_link_state, LINK_NONE);
  TEST_ASSERT_FALSE(lv_obj_has_flag(ui.btn_cure, LV_OBJ_FLAG_CLICKABLE));
  TEST_ASSERT_TRUE(lv_obj_has_state(ui.btn_cure, LV_STATE_DISABLED));
  click_center(ui.btn_cure);
  TEST_ASSERT_EQUAL_INT(NAV_NONE, lv_subject_get_int(&subj_nav_request));

  // Schema mismatch gates the tiles too.
  lv_subject_set_int(&subj_link_state, LINK_SCHEMA);
  TEST_ASSERT_FALSE(lv_obj_has_flag(ui.btn_cure, LV_OBJ_FLAG_CLICKABLE));
}

void test_link_state_updates_banner_and_indicator(void) {
  HomeScreen ui = create_home_screen(lv_screen_active());

  lv_subject_set_int(&subj_link_state, LINK_OK);
  TEST_ASSERT_TRUE(lv_obj_has_flag(ui.banner, LV_OBJ_FLAG_HIDDEN)); // hidden when healthy

  lv_subject_set_int(&subj_link_state, LINK_NONE);
  TEST_ASSERT_FALSE(lv_obj_has_flag(ui.banner, LV_OBJ_FLAG_HIDDEN)); // shown when link lost
}

void test_state_and_temp_drive_the_band(void) {
  HomeScreen ui = create_home_screen(lv_screen_active());

  lv_subject_set_int(&subj_run_state, RUN_IDLE);
  TEST_ASSERT_EQUAL_STRING("IDLE", lv_label_get_text(ui.state_label));

  lv_subject_set_int(&subj_run_state, RUN_HOT);
  TEST_ASSERT_EQUAL_STRING("HOT", lv_label_get_text(ui.state_label));

  lv_subject_set_int(&subj_chamber_temp, 118);
  TEST_ASSERT_EQUAL_STRING("Chamber 118 °C", lv_label_get_text(ui.chamber_label));
}

// The chamber readout follows the units setting (§24): changing subj_units re-renders it in °F,
// converting the stored °C value. This is the Home half of the "change units in Settings" flow.
void test_chamber_follows_units_setting(void) {
  HomeScreen ui = create_home_screen(lv_screen_active());
  lv_subject_set_int(&subj_chamber_temp, 100);
  TEST_ASSERT_EQUAL_STRING("Chamber 100 °C", lv_label_get_text(ui.chamber_label));
  lv_subject_set_int(&subj_units, 1); // °F: 100 °C -> 212 °F
  TEST_ASSERT_EQUAL_STRING("Chamber 212 °F", lv_label_get_text(ui.chamber_label));
  lv_subject_set_int(&subj_units, 0); // back to °C
  TEST_ASSERT_EQUAL_STRING("Chamber 100 °C", lv_label_get_text(ui.chamber_label));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_vm_state_pairs_word_and_colour);
  RUN_TEST(test_vm_link_gates_mode_and_pairs_word);
  RUN_TEST(test_vm_link_state_from_handshake);
  RUN_TEST(test_vm_dead_link_beats_a_latched_handshake);
  RUN_TEST(test_mode_tiles_publish_nav_intent);
  RUN_TEST(test_secondary_row_publishes_nav_intent);
  RUN_TEST(test_link_loss_disables_mode_tiles);
  RUN_TEST(test_link_state_updates_banner_and_indicator);
  RUN_TEST(test_state_and_temp_drive_the_band);
  RUN_TEST(test_chamber_follows_units_setting);
  return UNITY_END();
}
