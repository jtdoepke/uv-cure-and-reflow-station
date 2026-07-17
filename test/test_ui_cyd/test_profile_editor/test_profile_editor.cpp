// native_ui_cyd suite — the §12 profile editor (C5). Drives the real ProfileEditorScreen over a
// FakeProfileStorage-backed ProfileStore through the same seams the buttons invoke (openPhase /
// onFieldOpen / the numeric-keypad VM / onSave / commitName / the advanced structure methods), so
// the assertions are geometry-independent and run at both panel sizes. Pixel layout is reviewed via
// `make sim-shot`; here we pin behaviour. Same file-scope-statics lifetime discipline as
// test_profile_library (the screen's SelectableListModel owns the lv_subject_t the widgets
// observe).
#include <cstring>

#include <unity.h>
#include <lvgl.h>
#include "src/debugging/test/lv_test.h"

#include "helpers/fake_profile_storage.h"
#include "oven_cal.h"
#include "panel.h"
#include "profile_editor_screen.h"
#include "profile_store.h"
#include "profile_templates.h"
#include "subjects.h"

using Page = ProfileEditorScreen::Page;

static FakeProfileStorage reflow_fs;
static FakeProfileStorage cure_fs;
static ProfileStore reflow_store(reflow_fs, RecipeMode::Reflow);
static ProfileStore cure_store(cure_fs, RecipeMode::Cure);
static ProfileEditorScreen editor;

static bool g_exited;
static void on_exit(void *) {
  g_exited = true;
}

void setUp(void) {
  lv_init();
  lv_test_display_create(panel::W, panel::H);
  lv_test_indev_create_all();
  ui_subjects_init();
  reflow_fs.entries.clear();
  cure_fs.entries.clear();
  g_exited = false;
  editor.setExitHandler(on_exit, nullptr);
}
void tearDown(void) {
  lv_deinit();
}

// Begin editing `p` against `store`, render onto the active screen. `saveAs` forces name entry.
static void begin(ProfileStore &store, const ProfileStore::StoredProfile &p, bool saveAs) {
  editor.beginEdit(p, store, saveAs);
  editor.render(lv_screen_active());
}

// Type `value` into the open numeric editor and commit it (clear, then the digits, then OK).
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
  begin(reflow_store, profile_templates::defaultTemplate(RecipeMode::Reflow), /*saveAs=*/true);
  TEST_ASSERT_EQUAL_INT((int)Page::Overview, (int)editor.page());
  TEST_ASSERT_TRUE(editor.hardValid());
  // Default (uncalibrated) constants → the whole preview is idealized, so amber is expected.
  TEST_ASSERT_TRUE(editor.hasAmber());
  TEST_ASSERT_EQUAL_UINT(profile_templates::kReflowPhases, editor.working().phaseCount);
}

void test_overview_to_phase_to_field_and_back(void) {
  begin(reflow_store, profile_templates::defaultTemplate(RecipeMode::Reflow), true);
  editor.openPhase(1); // Soak
  TEST_ASSERT_EQUAL_INT((int)Page::PhaseEditor, (int)editor.page());
  TEST_ASSERT_EQUAL_INT(1, editor.selectedPhase());
  editor.onFieldOpen(1); // Target row (row 0 is Name)
  TEST_ASSERT_EQUAL_INT((int)Page::FieldEditor, (int)editor.page());
  editor.back(); // field → phase editor
  TEST_ASSERT_EQUAL_INT((int)Page::PhaseEditor, (int)editor.page());
  editor.back(); // phase editor → overview
  TEST_ASSERT_EQUAL_INT((int)Page::Overview, (int)editor.page());
}

// --- field editing ---

void test_edit_target_via_keypad(void) {
  begin(reflow_store, profile_templates::defaultTemplate(RecipeMode::Reflow), true);
  editor.openPhase(0);   // Preheat, target 150
  editor.onFieldOpen(1); // Target row (row 0 is Name)
  keypad_enter(160);
  TEST_ASSERT_EQUAL_INT((int)Page::PhaseEditor, (int)editor.page());
  TEST_ASSERT_EQUAL_FLOAT(160.0f, editor.working().phases[0].targetC);
}

void test_target_keypad_clamps_to_mode_cap(void) {
  begin(reflow_store, profile_templates::defaultTemplate(RecipeMode::Reflow), true);
  editor.openPhase(0);
  editor.onFieldOpen(1); // Target row (row 0 is Name)
  keypad_enter(999);     // reflow cap default 250 — the keypad refuses digits past max
  TEST_ASSERT_LESS_OR_EQUAL_FLOAT(250.0f, editor.working().phases[0].targetC);
  TEST_ASSERT_TRUE(editor.hardValid());
}

void test_fan_cycles_auto_on_off(void) {
  begin(reflow_store, profile_templates::defaultTemplate(RecipeMode::Reflow), true);
  editor.openPhase(0);
  TEST_ASSERT_EQUAL_INT((int)FanMode::Auto, (int)editor.working().phases[0].convFan);
  editor.onFieldOpen(4); // Conv fan row (Name, Target, Ramp, Hold, ConvFan)
  TEST_ASSERT_EQUAL_INT((int)FanMode::On, (int)editor.working().phases[0].convFan);
  editor.onFieldOpen(4);
  TEST_ASSERT_EQUAL_INT((int)FanMode::Off, (int)editor.working().phases[0].convFan);
  editor.onFieldOpen(4);
  TEST_ASSERT_EQUAL_INT((int)FanMode::Auto, (int)editor.working().phases[0].convFan);
}

void test_cure_uv_motor_toggle(void) {
  begin(cure_store, profile_templates::defaultTemplate(RecipeMode::Cure), true);
  editor.openPhase(0); // Warm — uv off by default
  TEST_ASSERT_FALSE(editor.working().phases[0].uv);
  editor.onFieldOpen(5); // UV row (cure-only; after Name/Target/Ramp/Hold/ConvFan)
  TEST_ASSERT_TRUE(editor.working().phases[0].uv);
  editor.onFieldOpen(6); // Motor row
  TEST_ASSERT_TRUE(editor.working().phases[0].motor);
}

// --- phase rename (Name row → keyboard) ---

void test_rename_phase_via_name_row(void) {
  begin(reflow_store, profile_templates::defaultTemplate(RecipeMode::Reflow), true);
  editor.openPhase(1); // Soak
  TEST_ASSERT_EQUAL_STRING("Soak", editor.working().phases[1].name);
  editor.onFieldOpen(0); // Name row → free-text keyboard
  TEST_ASSERT_EQUAL_INT((int)Page::NameEntry, (int)editor.page());
  editor.commitName("Bake");
  // Rename returns to the phase editor (not a Save) and updates only the target phase.
  TEST_ASSERT_EQUAL_INT((int)Page::PhaseEditor, (int)editor.page());
  TEST_ASSERT_EQUAL_STRING("Bake", editor.working().phases[1].name);
  TEST_ASSERT_EQUAL_STRING("Preheat", editor.working().phases[0].name);
  TEST_ASSERT_FALSE(editor.savedOk()); // a rename does not commit the profile
}

void test_empty_phase_name_rejected(void) {
  begin(reflow_store, profile_templates::defaultTemplate(RecipeMode::Reflow), true);
  editor.openPhase(0);
  editor.onFieldOpen(0); // Name row
  editor.commitName(""); // empty → validPhaseName rejects; stay on the keyboard
  TEST_ASSERT_EQUAL_INT((int)Page::NameEntry, (int)editor.page());
  TEST_ASSERT_EQUAL_STRING("Preheat", editor.working().phases[0].name); // unchanged
}

// The keyboard has no cancel key; the header Back cancels — a phase rename must return to the phase
// editor (not the overview) and leave the name untouched.
void test_phase_rename_back_returns_to_phase_editor(void) {
  begin(reflow_store, profile_templates::defaultTemplate(RecipeMode::Reflow), true);
  editor.openPhase(1);   // Soak
  editor.onFieldOpen(0); // Name row → keyboard
  TEST_ASSERT_EQUAL_INT((int)Page::NameEntry, (int)editor.page());
  editor.back(); // header Back cancels the rename
  TEST_ASSERT_EQUAL_INT((int)Page::PhaseEditor, (int)editor.page());
  TEST_ASSERT_EQUAL_STRING("Soak", editor.working().phases[1].name); // unchanged
}

// A profile-name (Save-as) entry's Back returns to the overview instead.
void test_profile_name_back_returns_to_overview(void) {
  begin(reflow_store, profile_templates::defaultTemplate(RecipeMode::Reflow), /*saveAs=*/true);
  editor.onSave(); // no name yet → name entry (naming_phase_ = -1)
  TEST_ASSERT_EQUAL_INT((int)Page::NameEntry, (int)editor.page());
  editor.back();
  TEST_ASSERT_EQUAL_INT((int)Page::Overview, (int)editor.page());
  TEST_ASSERT_FALSE(editor.savedOk());
}

// --- save ---

void test_named_save_writes_and_exits(void) {
  ProfileStore::StoredProfile p = profile_templates::defaultTemplate(RecipeMode::Reflow);
  std::strncpy(p.name, "MyProfile", kProfileNameCap - 1);
  begin(reflow_store, p, /*saveAs=*/false);
  editor.onSave(); // has a name, not save-as → commit directly
  TEST_ASSERT_TRUE(editor.savedOk());
  TEST_ASSERT_TRUE(g_exited);
  ProfileStore::StoredProfile loaded;
  TEST_ASSERT_TRUE(reflow_store.load("MyProfile", loaded));
  TEST_ASSERT_EQUAL_UINT(profile_templates::kReflowPhases, loaded.phaseCount);
}

void test_new_save_routes_through_name_entry(void) {
  begin(reflow_store, profile_templates::defaultTemplate(RecipeMode::Reflow), /*saveAs=*/true);
  editor.onSave(); // no name yet → name entry
  TEST_ASSERT_EQUAL_INT((int)Page::NameEntry, (int)editor.page());
  editor.commitName("Fresh");
  TEST_ASSERT_TRUE(editor.savedOk());
  TEST_ASSERT_TRUE(g_exited);
  ProfileStore::StoredProfile loaded;
  TEST_ASSERT_TRUE(reflow_store.load("Fresh", loaded));
  TEST_ASSERT_FALSE(loaded.stock);
}

void test_invalid_name_stays_on_name_entry(void) {
  begin(reflow_store, profile_templates::defaultTemplate(RecipeMode::Reflow), true);
  editor.onSave();
  editor.commitName("bad/name"); // path separator → validName rejects
  TEST_ASSERT_EQUAL_INT((int)Page::NameEntry, (int)editor.page());
  TEST_ASSERT_FALSE(editor.savedOk());
}

void test_hard_invalid_blocks_save(void) {
  ProfileStore::StoredProfile p = profile_templates::defaultTemplate(RecipeMode::Reflow);
  p.phases[0].targetC = 999.0f; // above the reflow cap → hard-invalid
  std::strncpy(p.name, "Bad", kProfileNameCap - 1);
  begin(reflow_store, p, /*saveAs=*/false);
  TEST_ASSERT_FALSE(editor.hardValid());
  editor.onSave();
  TEST_ASSERT_FALSE(editor.savedOk());
  TEST_ASSERT_FALSE(g_exited);
}

// --- advanced structure edits ---

void test_advanced_add_and_delete_phase(void) {
  begin(reflow_store, profile_templates::defaultTemplate(RecipeMode::Reflow), true);
  const size_t n0 = editor.working().phaseCount;
  editor.openPhase(0);
  editor.back(); // back to overview so selected_phase_ = 0
  editor.addPhase();
  TEST_ASSERT_EQUAL_UINT(n0 + 1, editor.working().phaseCount);
  editor.deletePhase();
  TEST_ASSERT_EQUAL_UINT(n0, editor.working().phaseCount);
}

void test_delete_never_below_one_phase(void) {
  ProfileStore::StoredProfile p = profile_templates::defaultTemplate(RecipeMode::Cure);
  p.phaseCount = 1; // a one-phase profile
  begin(cure_store, p, true);
  editor.deletePhase();
  TEST_ASSERT_EQUAL_UINT(1, editor.working().phaseCount);
}

void test_reorder_swaps_phases(void) {
  begin(reflow_store, profile_templates::defaultTemplate(RecipeMode::Reflow), true);
  const float t0 = editor.working().phases[0].targetC;
  const float t1 = editor.working().phases[1].targetC;
  editor.openPhase(0);
  editor.back();
  editor.movePhaseDown(); // phase 0 ↔ phase 1
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
  RUN_TEST(test_advanced_add_and_delete_phase);
  RUN_TEST(test_delete_never_below_one_phase);
  RUN_TEST(test_reorder_swaps_phases);
  return UNITY_END();
}
