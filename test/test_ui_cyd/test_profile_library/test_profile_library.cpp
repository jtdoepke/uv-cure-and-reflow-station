// native_ui_cyd suite — the §23 profile library screen (C4; rewired for Wave R3b of the §2 "CYD is
// a UI remote" split). Drives the real ProfileLibraryScreen against the REAL remote stack — a
// ManagementClient talking to a ManagementResponder + controller ProfileStores over a LoopbackPipe
// — through the same seams the footer/action buttons invoke. Because the list/detail/actions are
// now round-trips, each op is followed by settle() (pump the pipe + screen.poll() until the screen
// leaves its Loading state). Geometry-independent; runs at both panel sizes. Pixel layout is
// reviewed via `make sim-shot`.
#include <cstring>

#include <unity.h>
#include <lvgl.h>
#include "src/debugging/test/lv_test.h" // lv_test_display_create / indev (gated by LV_USE_TEST)

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
#include "profile_library.h"
#include "profile_library_screen.h"
#include "subjects.h"

using Page = ProfileLibraryScreen::Page;

// --- the remote stack (controller responder <-> CYD client over a pipe) ---
static LoopbackPipe pipe;
static FakeClock clk;
static FakeProfileStorage cure_fs;
static FakeProfileStorage reflow_fs;
static FakeSettingsStorage settings_fs;
static control::ProfileStore cure_store(cure_fs, oven_Mode_MODE_CURE);
static control::ProfileStore reflow_store(reflow_fs, oven_Mode_MODE_REFLOW);
static control::SettingsStore settings_store(settings_fs);
static protocol::MessageRouter ctrl_router;
static protocol::FrameLink ctrl_link(pipe.b(), TF_SLAVE, ctrl_router);
static ManagementResponder responder(ctrl_link, cure_store, reflow_store);
static protocol::MessageRouter cyd_router;
static protocol::FrameLink cyd_link(pipe.a(), TF_MASTER, cyd_router);
static ManagementClient client(cyd_link, clk);
static ProfileLibraryScreen screen;

static bool g_exited;
static void on_exit(void *) {
  g_exited = true;
}

// Pump both link directions + poll the screen until it leaves Loading (or a bound is hit). An
// action re-lists, so two round-trips can be needed — the loop covers it.
static void settle() {
  for (int i = 0; i < 12; ++i) {
    ctrl_link.poll();
    cyd_link.poll();
    client.service();
    screen.poll();
    if (screen.page() != Page::Loading) {
      return;
    }
  }
}

void setUp(void) {
  lv_init();
  lv_test_display_create(panel::W, panel::H);
  lv_test_indev_create_all();
  ui_subjects_init();
  cure_fs.entries.clear();
  reflow_fs.entries.clear();
  responder.reset(); // clear the dedup cache between tests
  responder.setSettingsStore(settings_store);
  ctrl_router.setObserver(responder);
  cyd_router.setObserver(client);
  client.clear();
  g_exited = false;
}
void tearDown(void) {
  lv_deinit();
}

// --- seeding (into the CONTROLLER's store now) ---

static void seed(control::ProfileStore &store, const char *name, bool stock, float peakC,
                 size_t phaseCount) {
  oven_Profile p = oven_Profile_init_zero;
  p.mode = store.mode();
  std::strncpy(p.name, name, sizeof(p.name) - 1);
  p.stock = stock;
  p.phases_count = static_cast<pb_size_t>(phaseCount);
  for (size_t i = 0; i < phaseCount && i < 32; ++i) {
    p.phases[i].target_c = (i + 1 == phaseCount) ? peakC : peakC * 0.6F; // last phase is the peak
    p.phases[i].ramp_s = 60.0F;
    p.phases[i].hold_s = 30.0F;
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

// Open a mode and wait for the list to arrive.
static void openMode(RecipeMode m) {
  screen.openMode(m);
  settle();
}

// --- navigation ---

void test_chooser_to_list_to_detail_and_back(void) {
  seed(reflow_store, "LF-245", false, 245.0f, 3);
  screen.setExitHandler(on_exit, nullptr);
  screen.begin(lv_screen_active(), client);
  TEST_ASSERT_EQUAL_INT((int)Page::Chooser, (int)screen.page());

  openMode(RecipeMode::Reflow);
  TEST_ASSERT_EQUAL_INT((int)Page::List, (int)screen.page());
  TEST_ASSERT_EQUAL_INT((int)RecipeMode::Reflow, (int)screen.mode());
  TEST_ASSERT_EQUAL_UINT(1, screen.vm().count());

  screen.listModel().select(0);
  screen.listModel().onOpen();
  settle();
  TEST_ASSERT_EQUAL_INT((int)Page::Detail, (int)screen.page());
  TEST_ASSERT_EQUAL_INT(0, screen.selected());

  screen.back(); // detail → list
  TEST_ASSERT_EQUAL_INT((int)Page::List, (int)screen.page());
  screen.back(); // list → chooser
  TEST_ASSERT_EQUAL_INT((int)Page::Chooser, (int)screen.page());
  screen.back(); // chooser → exit
  TEST_ASSERT_TRUE(g_exited);
}

void test_rows_alphabetical_with_facts(void) {
  seed(reflow_store, "SAC305", true, 249.0f, 2);
  seed(reflow_store, "LF-245", false, 245.0f, 3);
  screen.begin(lv_screen_active(), client);
  openMode(RecipeMode::Reflow);
  TEST_ASSERT_EQUAL_UINT(2, screen.vm().count());
  TEST_ASSERT_EQUAL_STRING("LF-245", screen.vm().name(0)); // alphabetical
  TEST_ASSERT_EQUAL_STRING("SAC305", screen.vm().name(1));
  TEST_ASSERT_NOT_NULL(std::strstr(screen.vm().rowValue(0), "peak")); // controller-computed facts
}

void test_stock_gating(void) {
  seed(reflow_store, "SAC305", true, 249.0f, 2);  // stock
  seed(reflow_store, "LF-245", false, 245.0f, 3); // user
  screen.begin(lv_screen_active(), client);
  openMode(RecipeMode::Reflow);
  const int stock = indexOf("SAC305");
  const int user = indexOf("LF-245");
  TEST_ASSERT_TRUE(screen.vm().editIsSaveAs(stock));
  TEST_ASSERT_FALSE(screen.vm().canDelete(stock));
  TEST_ASSERT_FALSE(screen.vm().editIsSaveAs(user));
  TEST_ASSERT_TRUE(screen.vm().canDelete(user));
}

void test_duplicate_creates_copy(void) {
  seed(reflow_store, "LF-245", false, 245.0f, 3);
  screen.begin(lv_screen_active(), client);
  openMode(RecipeMode::Reflow);
  screen.openDetail(indexOf("LF-245"));
  settle();
  screen.onDuplicate();
  settle();
  TEST_ASSERT_EQUAL_INT((int)Page::List, (int)screen.page());
  TEST_ASSERT_EQUAL_UINT(2, screen.vm().count());
  TEST_ASSERT_TRUE(indexOf("LF-245 copy") >= 0);
  TEST_ASSERT_FALSE(screen.vm().rowStock(indexOf("LF-245 copy")));
}

void test_rename_changes_name_and_refuses_clash(void) {
  seed(reflow_store, "LF-245", false, 245.0f, 3);
  seed(reflow_store, "SAC305", true, 249.0f, 2); // an existing name to clash against
  screen.begin(lv_screen_active(), client);
  openMode(RecipeMode::Reflow);
  screen.openDetail(indexOf("LF-245"));
  settle();

  screen.onRenameRequested();
  TEST_ASSERT_EQUAL_INT((int)Page::Rename, (int)screen.page());
  screen.back(); // cancel
  TEST_ASSERT_EQUAL_INT((int)Page::Detail, (int)screen.page());

  // Commit a new name → back to the list, renamed.
  screen.onRenameRequested();
  screen.onRenameCommit("LF-250");
  settle();
  TEST_ASSERT_EQUAL_INT((int)Page::List, (int)screen.page());
  TEST_ASSERT_TRUE(indexOf("LF-245") < 0);
  TEST_ASSERT_TRUE(indexOf("LF-250") >= 0);

  // A clash with an existing name is rejected client-side: stay on the Rename page, name unchanged.
  screen.openDetail(indexOf("LF-250"));
  settle();
  screen.onRenameRequested();
  screen.onRenameCommit("SAC305"); // taken
  TEST_ASSERT_EQUAL_INT((int)Page::Rename, (int)screen.page());
  TEST_ASSERT_TRUE(indexOf("LF-250") >= 0);
}

void test_delete_confirm_removes_and_cancel_keeps(void) {
  seed(reflow_store, "LF-245", false, 245.0f, 3);
  seed(reflow_store, "Keep", false, 200.0f, 2);
  screen.begin(lv_screen_active(), client);
  openMode(RecipeMode::Reflow);
  screen.openDetail(indexOf("LF-245"));
  settle();

  // Cancel path.
  screen.onDeleteRequested();
  TEST_ASSERT_EQUAL_INT((int)Page::ConfirmDelete, (int)screen.page());
  screen.back();
  TEST_ASSERT_EQUAL_INT((int)Page::Detail, (int)screen.page());

  // Confirm path.
  screen.onDeleteRequested();
  screen.onDeleteConfirmed();
  settle();
  TEST_ASSERT_EQUAL_INT((int)Page::List, (int)screen.page());
  TEST_ASSERT_EQUAL_UINT(1, screen.vm().count());
  TEST_ASSERT_TRUE(indexOf("LF-245") < 0);
  TEST_ASSERT_TRUE(indexOf("Keep") >= 0);
}

void test_empty_state(void) {
  screen.begin(lv_screen_active(), client);
  openMode(RecipeMode::Reflow); // empty store
  TEST_ASSERT_EQUAL_INT((int)Page::List, (int)screen.page());
  TEST_ASSERT_EQUAL_UINT(0, screen.vm().count());
  TEST_ASSERT_FALSE(screen.listModel().canOpen());
}

void test_open_enabled_with_profiles(void) {
  seed(reflow_store, "LF-245", false, 245.0f, 3);
  screen.begin(lv_screen_active(), client);
  openMode(RecipeMode::Reflow);
  TEST_ASSERT_TRUE(screen.listModel().canOpen());
}

void test_new_edit_publish_nav_intents(void) {
  seed(reflow_store, "LF-245", false, 245.0f, 3);
  screen.begin(lv_screen_active(), client);
  openMode(RecipeMode::Reflow);
  screen.openDetail(0);
  settle();

  lv_subject_set_int(&subj_nav_request, NAV_NONE);
  screen.onNew();
  TEST_ASSERT_EQUAL_INT(NAV_PROFILE_NEW, lv_subject_get_int(&subj_nav_request));
  screen.onEdit();
  TEST_ASSERT_EQUAL_INT(NAV_PROFILE_EDIT, lv_subject_get_int(&subj_nav_request));
}

// A controller that never answers → the screen surfaces the error state rather than hanging.
void test_link_down_shows_error(void) {
  screen.begin(lv_screen_active(), client);
  screen.openMode(RecipeMode::Reflow);
  TEST_ASSERT_EQUAL_INT((int)Page::Loading, (int)screen.page());
  // Never pump the controller; drive the client past its retry budget.
  for (int i = 0; i < 5; ++i) {
    clk.advance(1000);
    client.service();
    screen.poll();
  }
  TEST_ASSERT_EQUAL_INT((int)Page::Error, (int)screen.page());
  screen.back(); // error → chooser (return_page_)
  TEST_ASSERT_EQUAL_INT((int)Page::Chooser, (int)screen.page());
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_chooser_to_list_to_detail_and_back);
  RUN_TEST(test_rows_alphabetical_with_facts);
  RUN_TEST(test_stock_gating);
  RUN_TEST(test_duplicate_creates_copy);
  RUN_TEST(test_rename_changes_name_and_refuses_clash);
  RUN_TEST(test_delete_confirm_removes_and_cancel_keeps);
  RUN_TEST(test_empty_state);
  RUN_TEST(test_open_enabled_with_profiles);
  RUN_TEST(test_new_edit_publish_nav_intents);
  RUN_TEST(test_link_down_shows_error);
  return UNITY_END();
}
