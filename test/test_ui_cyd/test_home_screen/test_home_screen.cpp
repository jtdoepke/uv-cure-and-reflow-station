// native_ui_cyd suite — LVGL 9.5 headless UI test. LVGL runs on the host with LV_USE_TEST=1;
// an in-memory dummy display + simulated pointer drive the real create_home_screen() widgets.
// No board, no pixels on glass. LovyanGFX is not linked (lib_ignore in the env).
//
// Two layers of assertion: the view model's pure state→view mappers directly, and the rendered
// screen's behaviour (tile taps publish nav intents; link health gates the mode tiles; state
// changes update the band).
#include <initializer_list>

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

// The single status badge folds run-state AND link health into one word + colour (§14, revised).
// A healthy link with a cool idle chamber is the only path to green IDLE; a run is amber RUNNING; a
// hot idle chamber is amber HOT; a controller fault, an absent link, or a schema skew is red — each
// with its own word so the operator knows which fix (colour is never alone — §13/§14).
void test_vm_badge_pairs_word_and_colour(void) {
  // Healthy link: the badge tracks run-state.
  TEST_ASSERT_EQUAL_STRING("IDLE", HomeViewModel::badgeText(RUN_IDLE, LINK_OK));
  TEST_ASSERT_EQUAL_STRING("HOT", HomeViewModel::badgeText(RUN_HOT, LINK_OK));
  // RUNNING is its own amber word (a fail-safe): runStateFrom sets RUN_HOT only when idle, so a
  // cold chamber mid-run reads RUNNING, never a green IDLE and never a misleading "HOT".
  TEST_ASSERT_EQUAL_STRING("RUNNING", HomeViewModel::badgeText(RUN_RUNNING, LINK_OK));
  TEST_ASSERT_EQUAL_STRING("FAULT", HomeViewModel::badgeText(RUN_FAULT, LINK_OK));
  TEST_ASSERT_EQUAL_UINT32(theme::IDLE, HomeViewModel::badgeColor(RUN_IDLE, LINK_OK));
  TEST_ASSERT_EQUAL_UINT32(theme::WARN, HomeViewModel::badgeColor(RUN_HOT, LINK_OK));
  TEST_ASSERT_EQUAL_UINT32(theme::WARN, HomeViewModel::badgeColor(RUN_RUNNING, LINK_OK)); // amber
  TEST_ASSERT_EQUAL_UINT32(theme::FAULT, HomeViewModel::badgeColor(RUN_FAULT, LINK_OK));

  // A bad link wins over run-state: distinct word for each fix, red for both.
  TEST_ASSERT_EQUAL_STRING("NO LINK", HomeViewModel::badgeText(RUN_IDLE, LINK_NONE));
  TEST_ASSERT_EQUAL_STRING("SCHEMA", HomeViewModel::badgeText(RUN_IDLE, LINK_SCHEMA));
  TEST_ASSERT_EQUAL_UINT32(theme::FAULT, HomeViewModel::badgeColor(RUN_IDLE, LINK_NONE));
  TEST_ASSERT_EQUAL_UINT32(theme::FAULT, HomeViewModel::badgeColor(RUN_RUNNING, LINK_SCHEMA));
}

void test_vm_link_gates_mode(void) {
  TEST_ASSERT_TRUE(HomeViewModel::modeEnabled(LINK_OK));
  TEST_ASSERT_FALSE(HomeViewModel::modeEnabled(LINK_NONE));
  TEST_ASSERT_FALSE(HomeViewModel::modeEnabled(LINK_SCHEMA));
}

// The chamber readout's digits are coloured by the actual °C (§14/§17): touch-safe is white, warm
// is amber, dangerous is red — thresholds at the implicit-cool-phase point and the burn line.
void test_vm_chamber_colour_tracks_temp(void) {
  TEST_ASSERT_EQUAL_UINT32(theme::TEXT, HomeViewModel::chamberColor(25));
  TEST_ASSERT_EQUAL_UINT32(theme::TEXT, HomeViewModel::chamberColor(42)); // just below touch-safe
  TEST_ASSERT_EQUAL_UINT32(theme::WARN, HomeViewModel::chamberColor(43)); // touch-safe boundary
  TEST_ASSERT_EQUAL_UINT32(theme::WARN, HomeViewModel::chamberColor(59));
  TEST_ASSERT_EQUAL_UINT32(theme::FAULT, HomeViewModel::chamberColor(60)); // burn line
  TEST_ASSERT_EQUAL_UINT32(theme::FAULT, HomeViewModel::chamberColor(200));
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

// Telemetry -> Home badge RunState (§14): fault outranks running outranks hot; idle-and-cool is
// the only path to plain IDLE. This is what the firmware feeds from each fresh Telemetry frame.
void test_vm_run_state_from_telemetry(void) {
  TEST_ASSERT_EQUAL_INT(RUN_IDLE, HomeViewModel::runStateFrom(false, false, false));
  TEST_ASSERT_EQUAL_INT(RUN_HOT, HomeViewModel::runStateFrom(false, false, true));
  TEST_ASSERT_EQUAL_INT(RUN_RUNNING, HomeViewModel::runStateFrom(true, false, false));
  TEST_ASSERT_EQUAL_INT(RUN_FAULT, HomeViewModel::runStateFrom(false, true, false));
  // Severity order: a fault masks running/hot; a run masks hot.
  TEST_ASSERT_EQUAL_INT(RUN_FAULT, HomeViewModel::runStateFrom(true, true, true));
  TEST_ASSERT_EQUAL_INT(RUN_RUNNING, HomeViewModel::runStateFrom(true, false, true));
}

// atRest is the single predicate shared by the green idle dot and the §17 sleep gate: the screen
// may sleep ONLY when the machine is idle-and-touch-safe. A run, a hot chamber, or a fault all
// make it false — every case in which the screen must stay lit. This is the mapper the firmware's
// sleep_allowed calls, so the sleep policy and the dot cannot drift.
void test_vm_at_rest_gates_sleep(void) {
  TEST_ASSERT_TRUE(HomeViewModel::atRest(RUN_IDLE));
  TEST_ASSERT_FALSE(HomeViewModel::atRest(RUN_HOT));     // chamber above touch-safe -> stay awake
  TEST_ASSERT_FALSE(HomeViewModel::atRest(RUN_RUNNING)); // a run in progress -> stay awake
  TEST_ASSERT_FALSE(
      HomeViewModel::atRest(RUN_FAULT)); // a fault must never hide behind a dark screen
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
  lv_subject_set_int(&subj_link_state, LINK_OK); // secondary tiles are link-gated now too
  HomeScreen ui = create_home_screen(lv_screen_active());

  click_center(ui.btn_profiles);
  TEST_ASSERT_EQUAL_INT(NAV_PROFILES, lv_subject_get_int(&subj_nav_request));
  click_center(ui.btn_calibrate);
  TEST_ASSERT_EQUAL_INT(NAV_CALIBRATE, lv_subject_get_int(&subj_nav_request));
  click_center(ui.btn_settings);
  TEST_ASSERT_EQUAL_INT(NAV_SETTINGS, lv_subject_get_int(&subj_nav_request));
}

void test_link_loss_disables_all_tiles(void) {
  HomeScreen ui = create_home_screen(lv_screen_active());

  // Healthy link: every hub tile clickable.
  lv_subject_set_int(&subj_link_state, LINK_OK);
  for (lv_obj_t *b :
       {ui.btn_cure, ui.btn_reflow, ui.btn_profiles, ui.btn_calibrate, ui.btn_settings}) {
    TEST_ASSERT_TRUE(lv_obj_has_flag(b, LV_OBJ_FLAG_CLICKABLE));
    TEST_ASSERT_FALSE(lv_obj_has_state(b, LV_STATE_DISABLED));
  }

  // Link lost: the whole hub greys out (nothing works without the controller — §2 UI remote), and a
  // tap on any tile is ignored.
  lv_subject_set_int(&subj_nav_request, NAV_NONE);
  lv_subject_set_int(&subj_link_state, LINK_NONE);
  for (lv_obj_t *b :
       {ui.btn_cure, ui.btn_reflow, ui.btn_profiles, ui.btn_calibrate, ui.btn_settings}) {
    TEST_ASSERT_FALSE(lv_obj_has_flag(b, LV_OBJ_FLAG_CLICKABLE));
    TEST_ASSERT_TRUE(lv_obj_has_state(b, LV_STATE_DISABLED));
  }
  click_center(ui.btn_profiles);
  click_center(ui.btn_settings);
  TEST_ASSERT_EQUAL_INT(NAV_NONE, lv_subject_get_int(&subj_nav_request));

  // Schema mismatch gates the tiles too.
  lv_subject_set_int(&subj_link_state, LINK_SCHEMA);
  TEST_ASSERT_FALSE(lv_obj_has_flag(ui.btn_settings, LV_OBJ_FLAG_CLICKABLE));
}

void test_link_state_updates_banner_and_indicator(void) {
  HomeScreen ui = create_home_screen(lv_screen_active());

  lv_subject_set_int(&subj_run_state, RUN_IDLE);
  lv_subject_set_int(&subj_link_state, LINK_OK);
  TEST_ASSERT_TRUE(lv_obj_has_flag(ui.banner, LV_OBJ_FLAG_HIDDEN));    // banner hidden when healthy
  TEST_ASSERT_EQUAL_STRING("IDLE", lv_label_get_text(ui.state_label)); // badge shows run-state

  // A lost link both raises the banner AND takes over the status badge — it is the only link
  // readout now, so link trouble must win the badge over whatever the run-state was.
  lv_subject_set_int(&subj_link_state, LINK_NONE);
  TEST_ASSERT_FALSE(lv_obj_has_flag(ui.banner, LV_OBJ_FLAG_HIDDEN)); // shown when link lost
  TEST_ASSERT_EQUAL_STRING("NO LINK", lv_label_get_text(ui.state_label));

  lv_subject_set_int(&subj_link_state, LINK_SCHEMA);
  TEST_ASSERT_EQUAL_STRING("SCHEMA", lv_label_get_text(ui.state_label));
}

void test_state_and_temp_drive_the_band(void) {
  HomeScreen ui = create_home_screen(lv_screen_active());
  lv_subject_set_int(&subj_link_state, LINK_OK); // a healthy link, so the badge tracks run-state

  lv_subject_set_int(&subj_run_state, RUN_IDLE);
  TEST_ASSERT_EQUAL_STRING("IDLE", lv_label_get_text(ui.state_label));

  lv_subject_set_int(&subj_run_state, RUN_HOT);
  TEST_ASSERT_EQUAL_STRING("HOT", lv_label_get_text(ui.state_label));

  // The band's readout carries the VALUE only: "CHAMBER" is a separate dim caption widget beside
  // it (§14's labelled-numeric column). The unit stays on the value — it is part of the datum, not
  // part of the label, and a number whose unit lives in a different widget is a misreading waiting
  // to happen. A 118 °C chamber is well past the burn line, so the digits render in FAULT red.
  lv_subject_set_int(&subj_chamber_temp, 118);
  TEST_ASSERT_EQUAL_STRING("118 °C", lv_label_get_text(ui.chamber_label));
  lv_color_t hot = lv_obj_get_style_text_color(ui.chamber_label, LV_PART_MAIN);
  TEST_ASSERT_EQUAL_UINT32(lv_color_to_u32(theme::col(theme::FAULT)), lv_color_to_u32(hot));

  // A touch-safe chamber renders in primary white.
  lv_subject_set_int(&subj_chamber_temp, 25);
  lv_color_t cool = lv_obj_get_style_text_color(ui.chamber_label, LV_PART_MAIN);
  TEST_ASSERT_EQUAL_UINT32(lv_color_to_u32(theme::col(theme::TEXT)), lv_color_to_u32(cool));
}

// The chamber readout follows the units setting (§24): changing subj_units re-renders it in °F,
// converting the stored °C value. This is the Home half of the "change units in Settings" flow.
void test_chamber_follows_units_setting(void) {
  HomeScreen ui = create_home_screen(lv_screen_active());
  lv_subject_set_int(&subj_chamber_temp, 100);
  TEST_ASSERT_EQUAL_STRING("100 °C", lv_label_get_text(ui.chamber_label));
  lv_subject_set_int(&subj_units, 1); // °F: 100 °C -> 212 °F
  TEST_ASSERT_EQUAL_STRING("212 °F", lv_label_get_text(ui.chamber_label));
  lv_subject_set_int(&subj_units, 0); // back to °C
  TEST_ASSERT_EQUAL_STRING("100 °C", lv_label_get_text(ui.chamber_label));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_vm_badge_pairs_word_and_colour);
  RUN_TEST(test_vm_link_gates_mode);
  RUN_TEST(test_vm_chamber_colour_tracks_temp);
  RUN_TEST(test_vm_link_state_from_handshake);
  RUN_TEST(test_vm_dead_link_beats_a_latched_handshake);
  RUN_TEST(test_vm_run_state_from_telemetry);
  RUN_TEST(test_vm_at_rest_gates_sleep);
  RUN_TEST(test_mode_tiles_publish_nav_intent);
  RUN_TEST(test_secondary_row_publishes_nav_intent);
  RUN_TEST(test_link_loss_disables_all_tiles);
  RUN_TEST(test_link_state_updates_banner_and_indicator);
  RUN_TEST(test_state_and_temp_drive_the_band);
  RUN_TEST(test_chamber_follows_units_setting);
  return UNITY_END();
}
