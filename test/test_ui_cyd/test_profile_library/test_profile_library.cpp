// native_ui_cyd suite — the §23 profile library screen (C4). Drives the real ProfileLibraryScreen
// over a FakeProfileStorage-backed ProfileStore, through the same seams the footer/action buttons
// invoke (the SelectableListModel open handler + the screen's action methods), so the assertions
// are geometry-independent and run at both panel sizes. The pixel-level layout is reviewed via
// `make sim-shot --screen profile-library`; here we pin behaviour.
//
// The stores + screen are file-scope statics (the test_settings_screen pattern): the screen's
// SelectableListModel owns the lv_subject_t the built widgets observe, and those widgets outlive
// the test body (they are deleted by tearDown's lv_deinit) — so the model must outlive tearDown
// too. Each test reseeds the storage and re-begin()s the screen.
#include <cstring>

#include <unity.h>
#include <lvgl.h>
#include "src/debugging/test/lv_test.h" // lv_test_display_create / indev (gated by LV_USE_TEST)

#include "helpers/fake_profile_storage.h"
#include "oven_cal.h"
#include "panel.h"
#include "profile_library_screen.h"
#include "profile_store.h"
#include "subjects.h"

static FakeProfileStorage cure_fs;
static FakeProfileStorage reflow_fs;
static ProfileStore cure(cure_fs, RecipeMode::Cure);
static ProfileStore reflow(reflow_fs, RecipeMode::Reflow);
static ProfileLibraryScreen screen;

static bool g_exited;
static void on_exit(void *) {
  g_exited = true;
}

void setUp(void) {
  lv_init();
  lv_test_display_create(panel::W, panel::H);
  lv_test_indev_create_all();
  ui_subjects_init();
  cure_fs.entries.clear();
  reflow_fs.entries.clear();
  g_exited = false;
}
void tearDown(void) {
  lv_deinit();
}

// --- seeding ---

static void seed(ProfileStore &store, const char *name, bool stock, float peakC,
                 size_t phaseCount) {
  ProfileStore::StoredProfile p;
  std::strncpy(p.name, name, kProfileNameCap - 1);
  p.name[kProfileNameCap - 1] = '\0';
  p.mode = store.mode();
  p.stock = stock;
  p.phaseCount = phaseCount;
  for (size_t i = 0; i < phaseCount && i < kMaxPhases; ++i) {
    p.phases[i].targetC = (i + 1 == phaseCount) ? peakC : peakC * 0.6f; // last phase is the peak
    p.phases[i].rampSeconds = 60.0f;
    p.phases[i].holdSeconds = 30.0f;
  }
  TEST_ASSERT_TRUE(store.save(p));
}

static int indexOf(const char *name) {
  for (size_t i = 0; i < screen.vm().count(); ++i) {
    if (std::strcmp(screen.vm().name(i), name) == 0) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

// --- navigation ---

void test_chooser_to_list_to_detail_and_back(void) {
  seed(reflow, "LF-245", false, 245.0f, 3);
  screen.setExitHandler(on_exit, nullptr);
  screen.begin(lv_screen_active(), cure, reflow);
  TEST_ASSERT_EQUAL_INT((int)ProfileLibraryScreen::Page::Chooser, (int)screen.page());

  // Select "Reflow profiles" (row 1) and Open through the model seam the footer Open button uses.
  screen.listModel().select(1);
  screen.listModel().onOpen();
  TEST_ASSERT_EQUAL_INT((int)ProfileLibraryScreen::Page::List, (int)screen.page());
  TEST_ASSERT_EQUAL_INT((int)RecipeMode::Reflow, (int)screen.mode());
  TEST_ASSERT_EQUAL_UINT(1, screen.vm().count());

  screen.listModel().select(0);
  screen.listModel().onOpen();
  TEST_ASSERT_EQUAL_INT((int)ProfileLibraryScreen::Page::Detail, (int)screen.page());
  TEST_ASSERT_EQUAL_INT(0, screen.selected());

  screen.back(); // detail → list
  TEST_ASSERT_EQUAL_INT((int)ProfileLibraryScreen::Page::List, (int)screen.page());
  screen.back(); // list → chooser
  TEST_ASSERT_EQUAL_INT((int)ProfileLibraryScreen::Page::Chooser, (int)screen.page());
  screen.back(); // chooser → exit
  TEST_ASSERT_TRUE(g_exited);
}

void test_rows_alphabetical_with_facts(void) {
  seed(reflow, "SAC305", true, 249.0f, 2);
  seed(reflow, "LF-245", false, 245.0f, 3);
  screen.begin(lv_screen_active(), cure, reflow);
  screen.openMode(RecipeMode::Reflow);
  TEST_ASSERT_EQUAL_UINT(2, screen.vm().count());
  TEST_ASSERT_EQUAL_STRING("LF-245", screen.vm().name(0)); // alphabetical
  TEST_ASSERT_EQUAL_STRING("SAC305", screen.vm().name(1));
  TEST_ASSERT_NOT_NULL(std::strstr(screen.vm().rowValue(0), "peak"));
}

void test_stock_gating(void) {
  seed(reflow, "SAC305", true, 249.0f, 2);  // stock
  seed(reflow, "LF-245", false, 245.0f, 3); // user
  screen.begin(lv_screen_active(), cure, reflow);
  screen.openMode(RecipeMode::Reflow);
  const int stock = indexOf("SAC305");
  const int user = indexOf("LF-245");
  TEST_ASSERT_TRUE(screen.vm().editIsSaveAs(stock)); // stock → Edit becomes Save-as
  TEST_ASSERT_FALSE(screen.vm().canDelete(stock));   // stock → Delete disabled
  TEST_ASSERT_FALSE(screen.vm().editIsSaveAs(user)); // user → plain Edit
  TEST_ASSERT_TRUE(screen.vm().canDelete(user));     // user → Delete enabled
}

void test_duplicate_creates_copy(void) {
  seed(reflow, "LF-245", false, 245.0f, 3);
  screen.begin(lv_screen_active(), cure, reflow);
  screen.openMode(RecipeMode::Reflow);
  screen.openDetail(indexOf("LF-245"));
  screen.onDuplicate();
  TEST_ASSERT_EQUAL_INT((int)ProfileLibraryScreen::Page::List, (int)screen.page());
  TEST_ASSERT_EQUAL_UINT(2, screen.vm().count());
  TEST_ASSERT_TRUE(indexOf("LF-245 copy") >= 0); // the copy exists, user-owned
  TEST_ASSERT_FALSE(screen.vm().rowStock(indexOf("LF-245 copy")));
}

void test_delete_confirm_removes_and_cancel_keeps(void) {
  seed(reflow, "LF-245", false, 245.0f, 3);
  seed(reflow, "Keep", false, 200.0f, 2);
  screen.begin(lv_screen_active(), cure, reflow);
  screen.openMode(RecipeMode::Reflow);
  screen.openDetail(indexOf("LF-245"));

  // Cancel path: request, then Back → detail, nothing removed.
  screen.onDeleteRequested();
  TEST_ASSERT_EQUAL_INT((int)ProfileLibraryScreen::Page::ConfirmDelete, (int)screen.page());
  screen.back();
  TEST_ASSERT_EQUAL_INT((int)ProfileLibraryScreen::Page::Detail, (int)screen.page());
  TEST_ASSERT_TRUE(indexOf("LF-245") >= 0);

  // Confirm path: request, then confirm → list, LF-245 gone.
  screen.onDeleteRequested();
  screen.onDeleteConfirmed();
  TEST_ASSERT_EQUAL_INT((int)ProfileLibraryScreen::Page::List, (int)screen.page());
  TEST_ASSERT_EQUAL_UINT(1, screen.vm().count());
  TEST_ASSERT_TRUE(indexOf("LF-245") < 0);
  TEST_ASSERT_TRUE(indexOf("Keep") >= 0);
}

void test_empty_state(void) {
  screen.begin(lv_screen_active(), cure, reflow);
  screen.openMode(RecipeMode::Reflow); // empty store
  TEST_ASSERT_EQUAL_INT((int)ProfileLibraryScreen::Page::List, (int)screen.page());
  TEST_ASSERT_EQUAL_UINT(0, screen.vm().count());
}

void test_new_edit_load_publish_nav_intents(void) {
  seed(reflow, "LF-245", false, 245.0f, 3);
  screen.begin(lv_screen_active(), cure, reflow);
  screen.openMode(RecipeMode::Reflow);
  screen.openDetail(0);

  lv_subject_set_int(&subj_nav_request, NAV_NONE);
  screen.onNew();
  TEST_ASSERT_EQUAL_INT(NAV_PROFILE_NEW, lv_subject_get_int(&subj_nav_request));
  screen.onEdit();
  TEST_ASSERT_EQUAL_INT(NAV_PROFILE_EDIT, lv_subject_get_int(&subj_nav_request));
  screen.onLoad();
  TEST_ASSERT_EQUAL_INT(NAV_PROFILE_LOAD, lv_subject_get_int(&subj_nav_request));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_chooser_to_list_to_detail_and_back);
  RUN_TEST(test_rows_alphabetical_with_facts);
  RUN_TEST(test_stock_gating);
  RUN_TEST(test_duplicate_creates_copy);
  RUN_TEST(test_delete_confirm_removes_and_cancel_keeps);
  RUN_TEST(test_empty_state);
  RUN_TEST(test_new_edit_load_publish_nav_intents);
  return UNITY_END();
}
