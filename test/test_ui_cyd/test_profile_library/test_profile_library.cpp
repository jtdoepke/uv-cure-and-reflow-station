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

// Pick-mode handoff (C6): the chosen profile's run working copy comes back here.
static bool g_picked;
static ProfileDraft g_pick_draft;
static void on_pick(void *, const ProfileDraft &d) {
  g_picked = true;
  g_pick_draft = d;
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
  g_picked = false;
  g_pick_draft = ProfileDraft{};
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

// Find the button/tile carrying a label with exact text `t` (recursively): returns the label's
// parent, i.e. the pressable widget. The link-gating tests assert its CLICKABLE/DISABLED flags,
// which the screens don't otherwise expose (the buttons are build-local).
static lv_obj_t *findByLabel(lv_obj_t *root, const char *t) {
  const uint32_t n = lv_obj_get_child_count(root);
  for (uint32_t i = 0; i < n; ++i) {
    lv_obj_t *c = lv_obj_get_child(root, i);
    if (lv_obj_check_type(c, &lv_label_class) && std::strcmp(lv_label_get_text(c), t) == 0) {
      return root;
    }
    if (lv_obj_t *hit = findByLabel(c, t)) {
      return hit;
    }
  }
  return nullptr;
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

// The chooser tiles fetch a mode's library from the controller (§9), so they gate on the link like
// Home's run tiles: greyed + non-clickable when down, re-enabling reactively on reconnect.
void test_chooser_tiles_gate_on_link(void) {
  screen.begin(lv_screen_active(), client); // lands on the chooser
  lv_subject_set_int(&subj_link_state, LINK_OK);
  lv_obj_t *cure = findByLabel(lv_screen_active(), "UV CURE PROFILES");
  TEST_ASSERT_NOT_NULL(cure);
  TEST_ASSERT_TRUE(lv_obj_has_flag(cure, LV_OBJ_FLAG_CLICKABLE));

  lv_subject_set_int(&subj_link_state, LINK_NONE);
  TEST_ASSERT_FALSE(lv_obj_has_flag(cure, LV_OBJ_FLAG_CLICKABLE));
  TEST_ASSERT_TRUE(lv_obj_has_state(cure, LV_STATE_DISABLED));

  lv_subject_set_int(&subj_link_state, LINK_OK); // reconnect re-enables
  TEST_ASSERT_TRUE(lv_obj_has_flag(cure, LV_OBJ_FLAG_CLICKABLE));
  TEST_ASSERT_FALSE(lv_obj_has_state(cure, LV_STATE_DISABLED));
}

// The detail actions (Delete/Rename/Clone/Edit) each issue a management request or open the async
// editor, so they gate on the link too. Clone is always enabled by content rules (works for stock
// and user), so it is the clean probe for the link gate.
void test_detail_actions_gate_on_link(void) {
  seed(reflow_store, "LF-245", false, 245.0f, 3);
  screen.begin(lv_screen_active(), client);
  openMode(RecipeMode::Reflow);
  screen.openDetail(indexOf("LF-245"));
  settle();

  lv_subject_set_int(&subj_link_state, LINK_OK);
  lv_obj_t *clone = findByLabel(lv_screen_active(), "Clone");
  TEST_ASSERT_NOT_NULL(clone);
  TEST_ASSERT_TRUE(lv_obj_has_flag(clone, LV_OBJ_FLAG_CLICKABLE));
  TEST_ASSERT_FALSE(lv_obj_has_state(clone, LV_STATE_DISABLED));

  lv_subject_set_int(&subj_link_state, LINK_NONE);
  TEST_ASSERT_FALSE(lv_obj_has_flag(clone, LV_OBJ_FLAG_CLICKABLE));
  TEST_ASSERT_TRUE(lv_obj_has_state(clone, LV_STATE_DISABLED));
}

// --- Pick mode (§19/C6): the Setup screen's "Load a profile" ---

// beginPick skips the chooser (Setup already picked the mode) and lands on the mode's list, ordered
// most-recently-used by default.
void test_pick_mode_enters_list_mru(void) {
  seed(reflow_store, "LF-245", false, 245.0f, 3);
  screen.setExitHandler(on_exit, nullptr);
  screen.setPickHandler(on_pick, nullptr);
  screen.beginPick(lv_screen_active(), client, RecipeMode::Reflow);
  settle();
  TEST_ASSERT_TRUE(screen.pickMode());
  TEST_ASSERT_EQUAL_INT((int)Page::List, (int)screen.page());
  TEST_ASSERT_EQUAL_INT((int)RecipeMode::Reflow, (int)screen.mode());
  TEST_ASSERT_TRUE(screen.vm().mruSort()); // MRU by default (§23)
}

// The sort toggle flips MRU⇄alpha and re-fetches (the controller sorts); the list survives.
void test_pick_sort_toggle_reloads(void) {
  seed(reflow_store, "LF-245", false, 245.0f, 3);
  screen.setPickHandler(on_pick, nullptr);
  screen.beginPick(lv_screen_active(), client, RecipeMode::Reflow);
  settle();
  TEST_ASSERT_TRUE(screen.vm().mruSort());

  screen.toggleSortAndReload();
  settle();
  TEST_ASSERT_FALSE(screen.vm().mruSort()); // now alphabetical
  TEST_ASSERT_EQUAL_INT((int)Page::List, (int)screen.page());
  TEST_ASSERT_EQUAL_UINT(1, screen.vm().count());
}

// Selecting a profile fetches it and hands the assembled run draft (name + mode + phases) straight
// to the pick handler — no detail-preview page, and the editor is never opened (§19/C6: the picker
// goes directly to Confirm, which shows the one preview graph).
void test_pick_use_hands_back_draft(void) {
  seed(reflow_store, "LF-245", false, 245.0f, 3);
  screen.setPickHandler(on_pick, nullptr);
  screen.beginPick(lv_screen_active(), client, RecipeMode::Reflow);
  settle();

  TEST_ASSERT_FALSE(g_picked);
  screen.listModel().select(indexOf("LF-245"));
  screen.listModel().onOpen(); // tap a profile → fetch → hand off (no detail page shown)
  settle();

  TEST_ASSERT_TRUE(g_picked);
  TEST_ASSERT_EQUAL_STRING("LF-245", g_pick_draft.name);
  TEST_ASSERT_EQUAL_INT((int)RecipeMode::Reflow, (int)g_pick_draft.mode);
  TEST_ASSERT_EQUAL_UINT(3, g_pick_draft.phaseCount);
}

// Back from the pick list exits to the caller (Setup) — there is no chooser to fall back to.
void test_pick_back_exits(void) {
  seed(reflow_store, "LF-245", false, 245.0f, 3);
  screen.setExitHandler(on_exit, nullptr);
  screen.setPickHandler(on_pick, nullptr);
  screen.beginPick(lv_screen_active(), client, RecipeMode::Reflow);
  settle();
  TEST_ASSERT_EQUAL_INT((int)Page::List, (int)screen.page());

  screen.back();
  TEST_ASSERT_TRUE(g_exited);
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
  RUN_TEST(test_chooser_tiles_gate_on_link);
  RUN_TEST(test_detail_actions_gate_on_link);
  RUN_TEST(test_pick_mode_enters_list_mru);
  RUN_TEST(test_pick_sort_toggle_reloads);
  RUN_TEST(test_pick_use_hands_back_draft);
  RUN_TEST(test_pick_back_exits);
  return UNITY_END();
}
