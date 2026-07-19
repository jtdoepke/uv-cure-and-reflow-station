// native_ui_cyd suite — the §22 fault / alarm overlay (C8). Drives FaultOverlay against a real
// FaultController: it appears unbidden on a raise, never auto-dismisses when the condition clears,
// supersedes in place rather than stacking, survives an unrecognized code, and routes the
// acknowledge. Geometry-independent; runs at both panel sizes. Pixel layout via `make sim-shot`.
#include <unity.h>

#include <lvgl.h>
#include "src/debugging/test/lv_test.h"

#include "fault_controller.h"
#include "fault_overlay.h"
#include "panel.h"
#include "subjects.h"

static FaultController fc;
static FaultOverlay overlay;

static bool g_acked;
static AckRoute g_route;
static void on_ack(void *, AckRoute r) {
  g_acked = true;
  g_route = r;
}

void setUp(void) {
  lv_init();
  lv_test_display_create(panel::W, panel::H);
  lv_test_indev_create_all();
  ui_subjects_init();
  fc = FaultController{};
  overlay = FaultOverlay{};
  overlay.begin(fc);
  overlay.setAckHandler(on_ack, nullptr);
  g_acked = false;
  g_route = AckRoute::None;
}
void tearDown(void) {
  lv_deinit();
}

// Nothing latched → nothing on screen, and the top layer must not be eating touches.
void test_hidden_when_clear(void) {
  overlay.poll();
  TEST_ASSERT_FALSE(overlay.visible());
  TEST_ASSERT_FALSE(lv_obj_has_flag(lv_layer_top(), LV_OBJ_FLAG_CLICKABLE));
}

// A controller Fault raises it unbidden — no navigation, no user action.
void test_raises_on_controller_fault(void) {
  fc.onControllerFault(1000, 0x1234, oven_FaultCode_FAULT_OVERTEMP_CHAMBER);
  overlay.poll();
  TEST_ASSERT_TRUE(overlay.visible());
  TEST_ASSERT_EQUAL_INT((int)oven_FaultCode_FAULT_OVERTEMP_CHAMBER, (int)overlay.shownCode());
  // Modal: the top layer absorbs touches, so the screen beneath cannot be operated.
  TEST_ASSERT_TRUE(lv_obj_has_flag(lv_layer_top(), LV_OBJ_FLAG_CLICKABLE));
}

// §22's headline rule: latching, never auto-dismiss — "even if the condition clears". Polling
// forever with a healthy link must not take the overlay down.
void test_never_auto_dismisses(void) {
  fc.setRunActive(true);
  fc.tick(1000, /*linkHealthy=*/false); // self-raise LINK_LOST during a run
  overlay.poll();
  TEST_ASSERT_TRUE(overlay.visible());

  for (uint32_t t = 2000; t < 20000; t += 1000) {
    fc.tick(t, /*linkHealthy=*/true); // the link came back...
    overlay.poll();
  }
  TEST_ASSERT_TRUE(overlay.visible()); // ...and the alarm is still there
}

// A higher-severity cause takes over the SAME panel with a +N count (§22: "don't stack modals").
void test_supersede_updates_in_place(void) {
  fc.onControllerFault(1000, 1, oven_FaultCode_FAULT_LINK_LOST);
  overlay.poll();
  TEST_ASSERT_EQUAL_INT((int)oven_FaultCode_FAULT_LINK_LOST, (int)overlay.shownCode());

  fc.onControllerFault(1100, 1, oven_FaultCode_FAULT_OVERTEMP_CHAMBER);
  overlay.poll();
  TEST_ASSERT_TRUE(overlay.visible());
  // The displayed cause moved to the real hazard — a LINK_LOST must never mask an over-temp.
  TEST_ASSERT_EQUAL_INT((int)oven_FaultCode_FAULT_OVERTEMP_CHAMBER, (int)overlay.shownCode());
  TEST_ASSERT_EQUAL_UINT32(2, fc.state().count);
  // Still exactly one panel on the layer, not two stacked.
  TEST_ASSERT_EQUAL_UINT32(1, lv_obj_get_child_count(lv_layer_top()));
}

// Acknowledge with a run active routes to the Run Summary, so the aborted run keeps its record.
void test_ack_routes_to_summary(void) {
  fc.setRunActive(true);
  fc.onControllerFault(1000, 1, oven_FaultCode_FAULT_OVERTEMP_CHAMBER);
  overlay.poll();
  overlay.acknowledge();

  TEST_ASSERT_TRUE(g_acked);
  TEST_ASSERT_EQUAL_INT((int)AckRoute::RunSummary, (int)g_route);
  TEST_ASSERT_FALSE(overlay.visible());
  // Input must go back to the app — a stuck clickable top layer would swallow every later touch.
  TEST_ASSERT_FALSE(lv_obj_has_flag(lv_layer_top(), LV_OBJ_FLAG_CLICKABLE));
}

// Acknowledge with no run active goes Home instead.
void test_ack_routes_home_when_idle(void) {
  fc.setRunActive(false);
  fc.onControllerFault(1000, 1, oven_FaultCode_FAULT_SENSOR_FAULT);
  overlay.poll();
  overlay.acknowledge();
  TEST_ASSERT_EQUAL_INT((int)AckRoute::Home, (int)g_route);
}

// §22 defence-in-depth: a code the table doesn't know still draws — it must never be blank, and
// must never pass a null string to LVGL (faultInfo returns nulls for an unrecognized code).
void test_unknown_code_still_draws(void) {
  const oven_FaultCode unknown = static_cast<oven_FaultCode>(9999);
  fc.onControllerFault(1000, 1, unknown);
  overlay.poll();
  TEST_ASSERT_TRUE(overlay.visible());
  TEST_ASSERT_EQUAL_INT((int)unknown, (int)overlay.shownCode());
}

// A second raise after an acknowledge is a NEW episode: it must come back, not stay dismissed.
void test_reraise_after_ack(void) {
  fc.onControllerFault(1000, 1, oven_FaultCode_FAULT_SENSOR_FAULT);
  overlay.poll();
  overlay.acknowledge();
  TEST_ASSERT_FALSE(overlay.visible());

  fc.onControllerFault(5000, 1, oven_FaultCode_FAULT_OVERTEMP_CHAMBER);
  overlay.poll();
  TEST_ASSERT_TRUE(overlay.visible());
  TEST_ASSERT_EQUAL_UINT32(1, fc.state().count); // a fresh episode, not a continuation
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_hidden_when_clear);
  RUN_TEST(test_raises_on_controller_fault);
  RUN_TEST(test_never_auto_dismisses);
  RUN_TEST(test_supersede_updates_in_place);
  RUN_TEST(test_ack_routes_to_summary);
  RUN_TEST(test_ack_routes_home_when_idle);
  RUN_TEST(test_unknown_code_still_draws);
  RUN_TEST(test_reraise_after_ack);
  return UNITY_END();
}
