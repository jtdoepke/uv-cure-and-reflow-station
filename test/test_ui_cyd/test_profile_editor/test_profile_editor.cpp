// native_ui_cyd suite — the §12 profile editor (C5; rewired for Wave R3b of the §2 "CYD is a UI
// remote" split). Drives the real ProfileEditorScreen through the button seams (openPhase /
// onFieldOpen / the keypad VM / onSave / commitName / advanced structure edits). Editing itself is
// still synchronous on an in-RAM ProfileDraft; only Save is a round-trip to the controller (a real
// ManagementResponder + control ProfileStores over a LoopbackPipe), so a successful save is
// followed by settle(). Geometry-independent; runs at both panel sizes.
#include <cstring>

#include <unity.h>
#include <lvgl.h>
#include "src/debugging/test/lv_test.h"

#include "device_settings.h"
#include "frame_link.h"
#include "helpers/fake_clock.h"
#include "helpers/fake_profile_storage.h"
#include "helpers/fake_settings_storage.h"
#include "helpers/pipe_transport.h"
#include "management_client.h"
#include "management_responder.h"
#include "message_router.h"
#include "oven.pb.h"
#include "panel.h"
#include "profile_draft.h"
#include "profile_editor_screen.h"
#include "profile_library.h"
#include "profile_templates.h"
#include "subjects.h"

using Page = ProfileEditorScreen::Page;

// --- the remote stack ---
static LoopbackPipe pipe;
static FakeClock clk;
static FakeProfileStorage reflow_fs;
static FakeProfileStorage cure_fs;
static FakeSettingsStorage settings_fs;
static control::ProfileStore reflow_store(reflow_fs, oven_Mode_MODE_REFLOW);
static control::ProfileStore cure_store(cure_fs, oven_Mode_MODE_CURE);
static control::SettingsStore settings_store(settings_fs);
static protocol::MessageRouter ctrl_router;
static protocol::FrameLink ctrl_link(pipe.b(), TF_SLAVE, ctrl_router);
static ManagementResponder responder(ctrl_link, cure_store, reflow_store);
static protocol::MessageRouter cyd_router;
static protocol::FrameLink cyd_link(pipe.a(), TF_MASTER, cyd_router);
static ManagementClient client(cyd_link, clk);
static ProfileEditorScreen editor;

static bool g_exited;
static void on_exit(void *) {
  g_exited = true;
}

static void settle() {
  for (int i = 0; i < 12; ++i) {
    ctrl_link.poll();
    cyd_link.poll();
    client.service();
    editor.poll();
    if (editor.page() != Page::Loading) {
      return;
    }
  }
}

void setUp(void) {
  lv_init();
  lv_test_display_create(panel::W, panel::H);
  lv_test_indev_create_all();
  ui_subjects_init();
  reflow_fs.entries.clear();
  cure_fs.entries.clear();
  responder.reset();
  responder.setSettingsStore(settings_store);
  ctrl_router.setObserver(responder);
  cyd_router.setObserver(client);
  client.clear();
  g_exited = false;
  editor.setExitHandler(on_exit, nullptr);
}
void tearDown(void) {
  lv_deinit();
}

// Begin editing a fresh/named draft (a template, or a pre-built draft with a name) and render it.
static void begin(const ProfileDraft &p, bool saveAs) {
  editor.beginNew(p, client, saveAs);
  editor.render(lv_screen_active());
}

static void keypad_enter(int32_t value) {
  NumericKeypadViewModel &vm = editor.keypadVm();
  vm.onClear();
  char digits[16];
  std::snprintf(digits, sizeof(digits), "%d", value);
  for (const char *c = digits; *c; ++c) {
    if (*c >= '0' && *c <= '9') {
      vm.onDigit(*c - '0');
    }
  }
  vm.onOk();
}

// --- navigation ---

void test_template_opens_valid_on_overview(void) {
  begin(profile_templates::defaultTemplate(RecipeMode::Reflow), /*saveAs=*/true);
  TEST_ASSERT_EQUAL_INT((int)Page::Overview, (int)editor.page());
  TEST_ASSERT_TRUE(editor.hardValid());
  TEST_ASSERT_TRUE(editor.hasAmber()); // uncalibrated default → idealized preview
  TEST_ASSERT_EQUAL_UINT(profile_templates::kReflowPhases, editor.working().phaseCount);
}

void test_overview_to_phase_to_field_and_back(void) {
  begin(profile_templates::defaultTemplate(RecipeMode::Reflow), true);
  editor.openPhase(1);
  TEST_ASSERT_EQUAL_INT((int)Page::PhaseEditor, (int)editor.page());
  TEST_ASSERT_EQUAL_INT(1, editor.selectedPhase());
  editor.onFieldOpen(1);
  TEST_ASSERT_EQUAL_INT((int)Page::FieldEditor, (int)editor.page());
  editor.back();
  TEST_ASSERT_EQUAL_INT((int)Page::PhaseEditor, (int)editor.page());
  editor.back();
  TEST_ASSERT_EQUAL_INT((int)Page::Overview, (int)editor.page());
}

// --- field editing ---

void test_edit_target_via_keypad(void) {
  begin(profile_templates::defaultTemplate(RecipeMode::Reflow), true);
  editor.openPhase(0);
  editor.onFieldOpen(1);
  keypad_enter(160);
  TEST_ASSERT_EQUAL_INT((int)Page::PhaseEditor, (int)editor.page());
  TEST_ASSERT_EQUAL_FLOAT(160.0f, editor.working().phases[0].targetC);
}

void test_target_keypad_clamps_to_mode_cap(void) {
  begin(profile_templates::defaultTemplate(RecipeMode::Reflow), true);
  editor.openPhase(0);
  editor.onFieldOpen(1);
  keypad_enter(999);
  TEST_ASSERT_LESS_OR_EQUAL_FLOAT(250.0f, editor.working().phases[0].targetC);
  TEST_ASSERT_TRUE(editor.hardValid());
}

void test_fan_cycles_auto_on_off(void) {
  begin(profile_templates::defaultTemplate(RecipeMode::Reflow), true);
  editor.openPhase(0);
  TEST_ASSERT_EQUAL_INT((int)FanMode::Auto, (int)editor.working().phases[0].convFan);
  editor.onFieldOpen(4);
  TEST_ASSERT_EQUAL_INT((int)FanMode::On, (int)editor.working().phases[0].convFan);
  editor.onFieldOpen(4);
  TEST_ASSERT_EQUAL_INT((int)FanMode::Off, (int)editor.working().phases[0].convFan);
  editor.onFieldOpen(4);
  TEST_ASSERT_EQUAL_INT((int)FanMode::Auto, (int)editor.working().phases[0].convFan);
}

void test_cure_uv_motor_toggle(void) {
  begin(profile_templates::defaultTemplate(RecipeMode::Cure), true);
  editor.openPhase(0);
  TEST_ASSERT_FALSE(editor.working().phases[0].uv);
  editor.onFieldOpen(5);
  TEST_ASSERT_TRUE(editor.working().phases[0].uv);
  editor.onFieldOpen(6);
  TEST_ASSERT_TRUE(editor.working().phases[0].motor);
}

// --- phase rename ---

void test_rename_phase_via_name_row(void) {
  begin(profile_templates::defaultTemplate(RecipeMode::Reflow), true);
  editor.openPhase(1);
  TEST_ASSERT_EQUAL_STRING("Soak", editor.working().phases[1].name);
  editor.onFieldOpen(0);
  TEST_ASSERT_EQUAL_INT((int)Page::NameEntry, (int)editor.page());
  editor.commitName("Bake");
  TEST_ASSERT_EQUAL_INT((int)Page::PhaseEditor, (int)editor.page());
  TEST_ASSERT_EQUAL_STRING("Bake", editor.working().phases[1].name);
  TEST_ASSERT_EQUAL_STRING("Preheat", editor.working().phases[0].name);
  TEST_ASSERT_FALSE(editor.savedOk());
}

void test_empty_phase_name_rejected(void) {
  begin(profile_templates::defaultTemplate(RecipeMode::Reflow), true);
  editor.openPhase(0);
  editor.onFieldOpen(0);
  editor.commitName("");
  TEST_ASSERT_EQUAL_INT((int)Page::NameEntry, (int)editor.page());
  TEST_ASSERT_EQUAL_STRING("Preheat", editor.working().phases[0].name);
}

void test_phase_rename_back_returns_to_phase_editor(void) {
  begin(profile_templates::defaultTemplate(RecipeMode::Reflow), true);
  editor.openPhase(1);
  editor.onFieldOpen(0);
  TEST_ASSERT_EQUAL_INT((int)Page::NameEntry, (int)editor.page());
  editor.back();
  TEST_ASSERT_EQUAL_INT((int)Page::PhaseEditor, (int)editor.page());
  TEST_ASSERT_EQUAL_STRING("Soak", editor.working().phases[1].name);
}

void test_profile_name_back_returns_to_overview(void) {
  begin(profile_templates::defaultTemplate(RecipeMode::Reflow), /*saveAs=*/true);
  editor.onSave();
  TEST_ASSERT_EQUAL_INT((int)Page::NameEntry, (int)editor.page());
  editor.back();
  TEST_ASSERT_EQUAL_INT((int)Page::Overview, (int)editor.page());
  TEST_ASSERT_FALSE(editor.savedOk());
}

// --- save (now a round-trip to the controller's store) ---

void test_named_save_writes_and_exits(void) {
  ProfileDraft p = profile_templates::defaultTemplate(RecipeMode::Reflow);
  std::strncpy(p.name, "MyProfile", kProfileNameCap - 1);
  begin(p, /*saveAs=*/false);
  editor.onSave(); // has a name, not save-as → push directly
  settle();
  TEST_ASSERT_TRUE(editor.savedOk());
  TEST_ASSERT_TRUE(g_exited);
  oven_Profile loaded = oven_Profile_init_zero;
  TEST_ASSERT_TRUE(reflow_store.load("MyProfile", loaded));
  TEST_ASSERT_EQUAL_UINT(profile_templates::kReflowPhases, loaded.phases_count);
}

void test_new_save_routes_through_name_entry(void) {
  begin(profile_templates::defaultTemplate(RecipeMode::Reflow), /*saveAs=*/true);
  editor.onSave();
  TEST_ASSERT_EQUAL_INT((int)Page::NameEntry, (int)editor.page());
  editor.commitName("Fresh");
  settle();
  TEST_ASSERT_TRUE(editor.savedOk());
  TEST_ASSERT_TRUE(g_exited);
  oven_Profile loaded = oven_Profile_init_zero;
  TEST_ASSERT_TRUE(reflow_store.load("Fresh", loaded));
  TEST_ASSERT_FALSE(loaded.stock); // a saved profile is always user-owned
}

void test_invalid_name_stays_on_name_entry(void) {
  begin(profile_templates::defaultTemplate(RecipeMode::Reflow), true);
  editor.onSave();
  editor.commitName("bad/name"); // path separator → rejected client-side, no request
  TEST_ASSERT_EQUAL_INT((int)Page::NameEntry, (int)editor.page());
  TEST_ASSERT_FALSE(editor.savedOk());
}

void test_hard_invalid_blocks_save(void) {
  ProfileDraft p = profile_templates::defaultTemplate(RecipeMode::Reflow);
  p.phases[0].targetC = 999.0f; // above the reflow cap → hard-invalid
  std::strncpy(p.name, "Bad", kProfileNameCap - 1);
  begin(p, /*saveAs=*/false);
  TEST_ASSERT_FALSE(editor.hardValid());
  editor.onSave(); // guarded — no request issued
  TEST_ASSERT_FALSE(editor.savedOk());
  TEST_ASSERT_FALSE(g_exited);
}

// Editing an existing profile fetches it from the controller first.
void test_begin_existing_fetches(void) {
  oven_Profile seed = oven_Profile_init_zero;
  seed.mode = oven_Mode_MODE_REFLOW;
  std::strncpy(seed.name, "LF-245", sizeof(seed.name) - 1);
  seed.phases_count = 3;
  seed.phases[2].target_c = 245.0f;
  TEST_ASSERT_TRUE(reflow_store.save(seed));

  editor.beginExisting(oven_Mode_MODE_REFLOW, "LF-245", client, /*saveAs=*/false);
  editor.render(lv_screen_active());
  TEST_ASSERT_EQUAL_INT((int)Page::Loading, (int)editor.page());
  settle();
  TEST_ASSERT_EQUAL_INT((int)Page::Overview, (int)editor.page());
  TEST_ASSERT_EQUAL_STRING("LF-245", editor.working().name);
  TEST_ASSERT_EQUAL_UINT(3, editor.working().phaseCount);
}

// --- advanced structure edits ---

void test_advanced_add_and_delete_phase(void) {
  begin(profile_templates::defaultTemplate(RecipeMode::Reflow), true);
  const size_t n0 = editor.working().phaseCount;
  editor.openPhase(0);
  editor.back();
  editor.addPhase();
  TEST_ASSERT_EQUAL_UINT(n0 + 1, editor.working().phaseCount);
  editor.deletePhase();
  TEST_ASSERT_EQUAL_UINT(n0, editor.working().phaseCount);
}

void test_delete_never_below_one_phase(void) {
  ProfileDraft p = profile_templates::defaultTemplate(RecipeMode::Cure);
  p.phaseCount = 1;
  begin(p, true);
  editor.deletePhase();
  TEST_ASSERT_EQUAL_UINT(1, editor.working().phaseCount);
}

void test_reorder_swaps_phases(void) {
  begin(profile_templates::defaultTemplate(RecipeMode::Reflow), true);
  const float t0 = editor.working().phases[0].targetC;
  const float t1 = editor.working().phases[1].targetC;
  editor.openPhase(0);
  editor.back();
  editor.movePhaseDown();
  TEST_ASSERT_EQUAL_FLOAT(t1, editor.working().phases[0].targetC);
  TEST_ASSERT_EQUAL_FLOAT(t0, editor.working().phases[1].targetC);
  TEST_ASSERT_EQUAL_INT(1, editor.selectedPhase());
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_template_opens_valid_on_overview);
  RUN_TEST(test_overview_to_phase_to_field_and_back);
  RUN_TEST(test_edit_target_via_keypad);
  RUN_TEST(test_target_keypad_clamps_to_mode_cap);
  RUN_TEST(test_fan_cycles_auto_on_off);
  RUN_TEST(test_cure_uv_motor_toggle);
  RUN_TEST(test_rename_phase_via_name_row);
  RUN_TEST(test_empty_phase_name_rejected);
  RUN_TEST(test_phase_rename_back_returns_to_phase_editor);
  RUN_TEST(test_profile_name_back_returns_to_overview);
  RUN_TEST(test_named_save_writes_and_exits);
  RUN_TEST(test_new_save_routes_through_name_entry);
  RUN_TEST(test_invalid_name_stays_on_name_entry);
  RUN_TEST(test_hard_invalid_blocks_save);
  RUN_TEST(test_begin_existing_fetches);
  RUN_TEST(test_advanced_add_and_delete_phase);
  RUN_TEST(test_delete_never_below_one_phase);
  RUN_TEST(test_reorder_swaps_phases);
  return UNITY_END();
}
