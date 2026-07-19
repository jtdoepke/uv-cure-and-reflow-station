// native_ui_cyd suite — the §19 run-setup screen (C6a). Seam-driven and geometry-independent, with
// no controller client: Setup owns the run's working-copy ProfileDraft and publishes nav intents;
// the picker/editor/Confirm wiring it drives is the composition root's (main.cpp), exercised
// end-to-end on the bench. Runs at both panel sizes; pixel layout is reviewed via `make sim-shot`.
#include <cstring>

#include <unity.h>
#include <lvgl.h>
#include "src/debugging/test/lv_test.h" // lv_test_display_create / indev (gated by LV_USE_TEST)

#include "panel.h"
#include "profile_templates.h"
#include "setup_screen.h"
#include "subjects.h"

using Page = SetupScreen::Page;

static SetupScreen screen;
static bool g_exited;
static void on_exit(void *) {
  g_exited = true;
}

void setUp(void) {
  lv_init();
  lv_test_display_create(panel::W, panel::H);
  lv_test_indev_create_all();
  ui_subjects_init();
  screen.setExitHandler(on_exit, nullptr);
  g_exited = false;
  lv_subject_set_int(&subj_link_state, LINK_OK);
}
void tearDown(void) {
  lv_deinit();
}

static void render() {
  screen.render(lv_screen_active());
}

// A valid, uploadable reflow run draft (the calibrated default template, named).
static ProfileDraft validDraft() {
  ProfileDraft d = profile_templates::defaultTemplate(RecipeMode::Reflow);
  std::strncpy(d.name, "LF-245", kProfileNameCap - 1);
  return d;
}

// Entering a mode from Home starts a fresh session: no draft, the Empty page, the mode remembered.
void test_enter_mode_is_empty(void) {
  screen.enterMode(RecipeMode::Cure);
  render();
  TEST_ASSERT_EQUAL_INT((int)Page::Empty, (int)screen.page());
  TEST_ASSERT_FALSE(screen.hasDraft());
  TEST_ASSERT_EQUAL_INT((int)RecipeMode::Cure, (int)screen.mode());
}

// The Empty page's "Load a profile" publishes the pick intent (main.cpp opens the picker).
void test_empty_load_publishes_pick(void) {
  screen.enterMode(RecipeMode::Reflow);
  render();
  lv_subject_set_int(&subj_nav_request, NAV_NONE);
  screen.onLoad();
  TEST_ASSERT_EQUAL_INT(NAV_SETUP_PICK, lv_subject_get_int(&subj_nav_request));
}

// Adopting a chosen draft flips to Loaded and takes the profile's identity + mode.
void test_set_draft_loads(void) {
  screen.enterMode(RecipeMode::Reflow);
  screen.setDraft(validDraft());
  render();
  TEST_ASSERT_EQUAL_INT((int)Page::Loaded, (int)screen.page());
  TEST_ASSERT_TRUE(screen.hasDraft());
  TEST_ASSERT_EQUAL_STRING("LF-245", screen.draft().name);
}

// Readiness gate: a valid draft + a healthy link is ready; a dropped link is not (door-open joins
// in C7/PR5).
void test_ready_gates_on_link(void) {
  screen.enterMode(RecipeMode::Reflow);
  screen.setDraft(validDraft());
  render();
  lv_subject_set_int(&subj_link_state, LINK_OK);
  TEST_ASSERT_TRUE(screen.ready());
  lv_subject_set_int(&subj_link_state, LINK_NONE);
  TEST_ASSERT_FALSE(screen.ready());
}

// A hard-invalid draft is never ready, and Start guards the seam even with a healthy link.
void test_invalid_draft_not_ready(void) {
  ProfileDraft d = validDraft();
  d.phases[0].targetC = 9999.0f; // above the reflow cap → hard-invalid compile
  screen.enterMode(RecipeMode::Reflow);
  screen.setDraft(d);
  render();
  lv_subject_set_int(&subj_link_state, LINK_OK);
  TEST_ASSERT_FALSE(screen.ready());
  lv_subject_set_int(&subj_nav_request, NAV_NONE);
  screen.onStart(); // guarded — no intent published
  TEST_ASSERT_EQUAL_INT(NAV_NONE, lv_subject_get_int(&subj_nav_request));
}

// Start on a ready run advances to Confirm (C6b).
void test_start_publishes_when_ready(void) {
  screen.enterMode(RecipeMode::Reflow);
  screen.setDraft(validDraft());
  render();
  lv_subject_set_int(&subj_link_state, LINK_OK);
  lv_subject_set_int(&subj_nav_request, NAV_NONE);
  screen.onStart();
  TEST_ASSERT_EQUAL_INT(NAV_SETUP_START, lv_subject_get_int(&subj_nav_request));
}

// The Loaded action row publishes Edit / Save-as intents (main.cpp routes them to the editor).
void test_loaded_actions_publish(void) {
  screen.enterMode(RecipeMode::Reflow);
  screen.setDraft(validDraft());
  render();
  screen.onEdit();
  TEST_ASSERT_EQUAL_INT(NAV_SETUP_EDIT, lv_subject_get_int(&subj_nav_request));
  screen.onSaveAs();
  TEST_ASSERT_EQUAL_INT(NAV_SETUP_SAVE_AS, lv_subject_get_int(&subj_nav_request));
}

// Back exits to the caller (Home rebuilds).
void test_back_exits(void) {
  screen.enterMode(RecipeMode::Reflow);
  render();
  screen.back();
  TEST_ASSERT_TRUE(g_exited);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_enter_mode_is_empty);
  RUN_TEST(test_empty_load_publishes_pick);
  RUN_TEST(test_set_draft_loads);
  RUN_TEST(test_ready_gates_on_link);
  RUN_TEST(test_invalid_draft_not_ready);
  RUN_TEST(test_start_publishes_when_ready);
  RUN_TEST(test_loaded_actions_publish);
  RUN_TEST(test_back_exits);
  return UNITY_END();
}
