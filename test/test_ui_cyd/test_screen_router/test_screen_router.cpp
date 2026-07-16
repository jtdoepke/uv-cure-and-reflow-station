// native_ui_cyd suite — ScreenRouter behaviour on headless LVGL. Drives the real router against a
// dummy display: cached screens build once and stay resident, on-demand screens rebuild and are
// freed on leave, and the reset-on-show hook fires on every cached re-show (never on first build).
#include <unity.h>
#include <lvgl.h>
#include "src/debugging/test/lv_test.h"

#include "panel.h"
#include "screen_router.h"

namespace {

// One screen's instrumentation: how many times it was built / reset, and its last-built object.
struct Probe {
  int build_count = 0;
  int reset_count = 0;
  lv_obj_t *last_built = nullptr;
};

void probe_build(void *ctx, lv_obj_t *scr) {
  auto *p = static_cast<Probe *>(ctx);
  p->build_count++;
  p->last_built = scr;
  lv_label_create(scr); // minimal non-empty content
}

void probe_reset(void *ctx) {
  static_cast<Probe *>(ctx)->reset_count++;
}

// Ids for the two screens the tests wire up.
constexpr int kHome = 0;
constexpr int kOther = 1;

} // namespace

void setUp(void) {
  lv_init();
  lv_test_display_create(panel::W, panel::H);
  lv_test_indev_create_all();
}

void tearDown(void) {
  lv_deinit();
}

// First show of a screen builds it once and makes it the active screen.
void test_first_show_builds_and_activates(void) {
  ScreenRouter router;
  Probe home;
  router.define(kHome, probe_build, &home, /*cached=*/true, probe_reset);

  router.show(kHome);

  TEST_ASSERT_EQUAL_INT(1, home.build_count);
  TEST_ASSERT_EQUAL_INT(0, home.reset_count); // first build is not a re-show
  TEST_ASSERT_EQUAL_INT(kHome, router.current());
  TEST_ASSERT_EQUAL_PTR(home.last_built, lv_screen_active());
  TEST_ASSERT_TRUE(router.isResident(kHome));
}

// A cached screen is built exactly once; navigating back to it loads the resident instance and
// fires reset-on-show instead of rebuilding.
void test_cached_built_once_reset_on_reshow(void) {
  ScreenRouter router;
  Probe home, other;
  router.define(kHome, probe_build, &home, /*cached=*/true, probe_reset);
  router.define(kOther, probe_build, &other, /*cached=*/false);

  router.show(kHome);
  lv_obj_t *home_obj = router.currentObj();
  router.show(kOther);
  router.show(kHome);

  TEST_ASSERT_EQUAL_INT(1, home.build_count);           // never rebuilt
  TEST_ASSERT_EQUAL_INT(1, home.reset_count);           // reset ran on the re-show
  TEST_ASSERT_EQUAL_PTR(home_obj, router.currentObj()); // same resident object
  TEST_ASSERT_EQUAL_PTR(home_obj, lv_screen_active());
}

// An on-demand screen is rebuilt on every entry and its object is deleted when navigated away from.
void test_on_demand_rebuilds_and_frees(void) {
  ScreenRouter router;
  Probe home, other;
  router.define(kHome, probe_build, &home, /*cached=*/true, probe_reset);
  router.define(kOther, probe_build, &other, /*cached=*/false);

  router.show(kHome);
  router.show(kOther);
  lv_obj_t *first_other = other.last_built;
  TEST_ASSERT_TRUE(lv_obj_is_valid(first_other));

  router.show(kHome); // leaving the on-demand screen frees it
  TEST_ASSERT_FALSE(lv_obj_is_valid(first_other));
  TEST_ASSERT_FALSE(router.isResident(kOther));

  router.show(kOther); // re-entry rebuilds a fresh object (its address may reuse the freed block)
  TEST_ASSERT_EQUAL_INT(2, other.build_count);
  TEST_ASSERT_TRUE(lv_obj_is_valid(other.last_built));
}

// Leaving a cached screen keeps it resident and valid (it is not deleted on leave).
void test_cached_survives_leave(void) {
  ScreenRouter router;
  Probe home, other;
  router.define(kHome, probe_build, &home, /*cached=*/true, probe_reset);
  router.define(kOther, probe_build, &other, /*cached=*/false);

  router.show(kHome);
  lv_obj_t *home_obj = home.last_built;
  router.show(kOther);

  TEST_ASSERT_TRUE(lv_obj_is_valid(home_obj));
  TEST_ASSERT_TRUE(router.isResident(kHome));
}

// Re-showing the current cached screen re-runs reset and never deletes it.
void test_reshow_current_is_safe(void) {
  ScreenRouter router;
  Probe home;
  router.define(kHome, probe_build, &home, /*cached=*/true, probe_reset);

  router.show(kHome);
  lv_obj_t *home_obj = home.last_built;
  router.show(kHome);

  TEST_ASSERT_EQUAL_INT(1, home.build_count);
  TEST_ASSERT_EQUAL_INT(1, home.reset_count);
  TEST_ASSERT_TRUE(lv_obj_is_valid(home_obj));
  TEST_ASSERT_EQUAL_PTR(home_obj, lv_screen_active());
}

// A cached screen with no reset hook (a stateless one, like Home) re-shows without a hook and is
// still built only once.
void test_cached_without_reset_hook(void) {
  ScreenRouter router;
  Probe home, other;
  router.define(kHome, probe_build, &home, /*cached=*/true, /*reset=*/nullptr);
  router.define(kOther, probe_build, &other, /*cached=*/false);

  router.show(kHome);
  router.show(kOther);
  router.show(kHome);

  TEST_ASSERT_EQUAL_INT(1, home.build_count);
  TEST_ASSERT_EQUAL_INT(0, home.reset_count);
  TEST_ASSERT_EQUAL_PTR(home.last_built, lv_screen_active());
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_first_show_builds_and_activates);
  RUN_TEST(test_cached_built_once_reset_on_reshow);
  RUN_TEST(test_on_demand_rebuilds_and_frees);
  RUN_TEST(test_cached_survives_leave);
  RUN_TEST(test_reshow_current_is_safe);
  RUN_TEST(test_cached_without_reset_hook);
  return UNITY_END();
}
