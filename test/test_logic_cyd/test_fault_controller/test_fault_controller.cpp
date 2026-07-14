// native_logic_cyd suite — pure host tests of the §22 fault latch + ack routing.
// No LVGL/Arduino: time and the `linkHealthy` predicate are passed in (the SleepController idiom).
#include <unity.h>

#include "fault_controller.h"

void setUp(void) {}
void tearDown(void) {}

namespace {
// A controller that has raised a fault mid-run, ready to acknowledge.
FaultController latchedDuringRun(uint32_t nowMs, oven_FaultCode code) {
  FaultController fc;
  fc.setRunActive(true);
  fc.onControllerFault(nowMs, /*session=*/7, code);
  return fc;
}
} // namespace

void test_starts_clear(void) {
  FaultController fc;
  TEST_ASSERT_FALSE(fc.active());
  const FaultState s = fc.state();
  TEST_ASSERT_EQUAL_UINT32(0, s.count);
  TEST_ASSERT_FALSE(s.overTemp);
  TEST_ASSERT_EQUAL(oven_FaultCode_FAULT_NONE, s.code);
}

void test_controller_fault_latches(void) {
  FaultController fc;
  fc.onControllerFault(1000, /*session=*/42, oven_FaultCode_FAULT_SENSOR_FAULT);
  TEST_ASSERT_TRUE(fc.active());
  const FaultState s = fc.state();
  TEST_ASSERT_EQUAL(oven_FaultCode_FAULT_SENSOR_FAULT, s.code);
  TEST_ASSERT_EQUAL_UINT32(42, s.session);
  TEST_ASSERT_EQUAL_UINT32(1, s.count);
  TEST_ASSERT_EQUAL_UINT32(1000, s.raisedAtMs);
}

// A `Fault` frame carrying the enum's zero value is malformed — don't latch a blank overlay.
void test_fault_none_is_ignored(void) {
  FaultController fc;
  fc.onControllerFault(1000, 42, oven_FaultCode_FAULT_NONE);
  TEST_ASSERT_FALSE(fc.active());
  TEST_ASSERT_EQUAL_UINT32(0, fc.state().count);
}

// §22's headline rule: "Latching — never auto-dismiss. The overlay stays until an explicit
// Acknowledge, **even if the condition clears** (e.g. the link returns)."
void test_never_auto_dismisses_when_condition_clears(void) {
  FaultController fc;
  fc.setRunActive(true);
  fc.tick(0, /*linkHealthy=*/true);
  fc.tick(100, /*linkHealthy=*/false); // link drops → self-raise
  TEST_ASSERT_TRUE(fc.active());
  for (uint32_t t = 200; t < 60000; t += 100) {
    fc.tick(t, /*linkHealthy=*/true); // link is back and healthy for a minute
  }
  TEST_ASSERT_TRUE(fc.active()); // still latched — only acknowledge() clears it
}

// §22: "Higher-priority fault while shown: update the overlay to the new cause and keep a +N
// count; don't stack modals."
void test_higher_priority_fault_replaces_cause_and_bumps_count(void) {
  FaultController fc;
  fc.onControllerFault(1000, 1, oven_FaultCode_FAULT_TARGET_UNREACHABLE);
  fc.onControllerFault(2000, 2, oven_FaultCode_FAULT_OVERTEMP_CHAMBER);
  const FaultState s = fc.state();
  TEST_ASSERT_EQUAL(oven_FaultCode_FAULT_OVERTEMP_CHAMBER, s.code);
  TEST_ASSERT_EQUAL_UINT32(2, s.session);       // session follows the displayed cause
  TEST_ASSERT_EQUAL_UINT32(2, s.count);         // the view renders "+1"
  TEST_ASSERT_EQUAL_UINT32(1000, s.raisedAtMs); // the episode began at the first fault
}

// The load-bearing consequence of the ordering: a LINK_LOST arriving after a real controller
// fault must not mask it. "Lost communication" would be strictly less informative than
// "Chamber over-temperature" — and §22's LINK_LOST wording claims nothing about live state.
void test_lower_priority_fault_only_bumps_count(void) {
  FaultController fc;
  fc.onControllerFault(1000, 1, oven_FaultCode_FAULT_OVERTEMP_CHAMBER);
  fc.onControllerFault(2000, 2, oven_FaultCode_FAULT_LINK_LOST);
  const FaultState s = fc.state();
  TEST_ASSERT_EQUAL(oven_FaultCode_FAULT_OVERTEMP_CHAMBER, s.code);
  TEST_ASSERT_EQUAL_UINT32(1, s.session); // session stays with the displayed cause
  TEST_ASSERT_EQUAL_UINT32(2, s.count);
}

void test_equal_severity_keeps_the_incumbent(void) {
  FaultController fc;
  fc.onControllerFault(1000, 1, oven_FaultCode_FAULT_OVERTEMP_CHAMBER);
  fc.onControllerFault(2000, 2, oven_FaultCode_FAULT_OVERTEMP_CASE); // same band
  TEST_ASSERT_EQUAL(oven_FaultCode_FAULT_OVERTEMP_CHAMBER, fc.state().code);
  TEST_ASSERT_EQUAL_UINT32(2, fc.state().count);
}

// The chamber over-temp is what leaves HOT behind; a later lower-priority fault takes over
// neither the cause nor the flag.
void test_overtemp_flag_is_sticky_across_a_lower_priority_update(void) {
  FaultController fc;
  fc.onControllerFault(1000, 1, oven_FaultCode_FAULT_OVERTEMP_CHAMBER);
  fc.onControllerFault(2000, 2, oven_FaultCode_FAULT_LINK_LOST);
  TEST_ASSERT_TRUE(fc.state().overTemp);
  TEST_ASSERT_TRUE(fc.overTempLatched());
}

// §22 scopes origin 2 to "CYD-detected link loss *during a run*". An idle link loss is the §13
// header indicator, not this modal — keeping the red overlay rare is what preserves its force.
void test_link_lost_self_raises_only_during_a_run(void) {
  FaultController idle;
  idle.setRunActive(false);
  idle.tick(0, true);
  idle.tick(100, /*linkHealthy=*/false);
  TEST_ASSERT_FALSE(idle.active());

  FaultController running;
  running.setRunActive(true);
  running.tick(0, true);
  running.tick(100, /*linkHealthy=*/false);
  TEST_ASSERT_TRUE(running.active());
  TEST_ASSERT_EQUAL(oven_FaultCode_FAULT_LINK_LOST, running.state().code);
}

// tick() runs at loop rate: a level-triggered self-raise would count thousands per second.
void test_link_lost_self_raise_is_edge_triggered(void) {
  FaultController fc;
  fc.setRunActive(true);
  fc.tick(0, true);
  for (uint32_t t = 1; t <= 1000; ++t) {
    fc.tick(t, /*linkHealthy=*/false);
  }
  TEST_ASSERT_EQUAL_UINT32(1, fc.state().count);
}

void test_link_recovery_rearms_the_self_raise_after_ack(void) {
  FaultController fc;
  fc.setRunActive(true);
  fc.tick(0, true);
  fc.tick(100, false); // raise #1
  TEST_ASSERT_EQUAL(AckRoute::RunSummary, fc.acknowledge());
  fc.tick(200, false); // still down, still armed-low → no new raise
  TEST_ASSERT_FALSE(fc.active());
  fc.tick(300, true);  // recovery re-arms
  fc.tick(400, false); // a fresh drop raises again
  TEST_ASSERT_TRUE(fc.active());
  TEST_ASSERT_EQUAL_UINT32(1, fc.state().count); // fresh episode, fresh count
}

void test_ack_during_a_run_routes_to_run_summary(void) {
  FaultController fc = latchedDuringRun(1000, oven_FaultCode_FAULT_OVERTEMP_CHAMBER);
  TEST_ASSERT_EQUAL(AckRoute::RunSummary, fc.acknowledge());
  TEST_ASSERT_FALSE(fc.active());
}

void test_ack_while_idle_routes_home(void) {
  FaultController fc;
  fc.setRunActive(false);
  fc.onControllerFault(1000, 1, oven_FaultCode_FAULT_INTERNAL);
  TEST_ASSERT_EQUAL(AckRoute::Home, fc.acknowledge());
}

// The race §22's routing rule depends on: the controller flips RUNNING→FAULT in the same breath
// as the Fault frame, so by ack time the telemetry says "not running". The aborted run must
// still get its record (§16), so the route is captured at raise, not at ack.
void test_ack_route_is_captured_at_raise_not_at_ack(void) {
  FaultController fc;
  fc.setRunActive(true);
  fc.onControllerFault(1000, 1, oven_FaultCode_FAULT_OVERTEMP_CHAMBER);
  fc.setRunActive(false); // telemetry now reports RUN_STATE_FAULT
  TEST_ASSERT_EQUAL(AckRoute::RunSummary, fc.acknowledge());
}

// §22: "Acknowledge is always allowed — it dismisses the *alarm*, not the *hazard* (which is
// already handled). We deliberately do not gate it on the condition clearing."
void test_ack_is_always_allowed_and_clears(void) {
  FaultController fc;
  fc.setRunActive(true);
  fc.tick(0, true);
  fc.tick(100, /*linkHealthy=*/false);
  TEST_ASSERT_TRUE(fc.active());
  TEST_ASSERT_EQUAL(AckRoute::RunSummary, fc.acknowledge()); // link still down
  TEST_ASSERT_FALSE(fc.active());
}

void test_ack_when_clear_is_a_no_op(void) {
  FaultController fc;
  TEST_ASSERT_EQUAL(AckRoute::None, fc.acknowledge());
  TEST_ASSERT_FALSE(fc.active());
}

// §22: "If the fault was over-temp, the HOT state (§14) persists on Home and keeps suppressing
// sleep (§17) until the chamber cools." The caller clears it when the chamber cools.
void test_overtemp_survives_acknowledge(void) {
  FaultController fc = latchedDuringRun(1000, oven_FaultCode_FAULT_OVERTEMP_CHAMBER);
  fc.acknowledge();
  TEST_ASSERT_TRUE(fc.overTempLatched());
  fc.clearOverTemp();
  TEST_ASSERT_FALSE(fc.overTempLatched());
}

void test_second_episode_after_ack_starts_a_fresh_count(void) {
  FaultController fc;
  fc.onControllerFault(1000, 1, oven_FaultCode_FAULT_OVERTEMP_CHAMBER);
  fc.onControllerFault(1100, 1, oven_FaultCode_FAULT_LINK_LOST);
  TEST_ASSERT_EQUAL_UINT32(2, fc.state().count);
  fc.acknowledge();
  fc.onControllerFault(2000, 2, oven_FaultCode_FAULT_INTERNAL);
  const FaultState s = fc.state();
  TEST_ASSERT_EQUAL_UINT32(1, s.count);
  TEST_ASSERT_EQUAL(oven_FaultCode_FAULT_INTERNAL, s.code); // not masked by the old over-temp
  TEST_ASSERT_EQUAL_UINT32(2000, s.raisedAtMs);
}

// C8's gateway polls state() and only touches lv_subject_* when updatedAtMs moved.
void test_updated_at_ms_bumps_on_every_raise(void) {
  FaultController fc;
  fc.onControllerFault(1000, 1, oven_FaultCode_FAULT_OVERTEMP_CHAMBER);
  TEST_ASSERT_EQUAL_UINT32(1000, fc.state().updatedAtMs);
  fc.onControllerFault(2500, 1, oven_FaultCode_FAULT_LINK_LOST); // lower priority, still an update
  TEST_ASSERT_EQUAL_UINT32(2500, fc.state().updatedAtMs);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_starts_clear);
  RUN_TEST(test_controller_fault_latches);
  RUN_TEST(test_fault_none_is_ignored);
  RUN_TEST(test_never_auto_dismisses_when_condition_clears);
  RUN_TEST(test_higher_priority_fault_replaces_cause_and_bumps_count);
  RUN_TEST(test_lower_priority_fault_only_bumps_count);
  RUN_TEST(test_equal_severity_keeps_the_incumbent);
  RUN_TEST(test_overtemp_flag_is_sticky_across_a_lower_priority_update);
  RUN_TEST(test_link_lost_self_raises_only_during_a_run);
  RUN_TEST(test_link_lost_self_raise_is_edge_triggered);
  RUN_TEST(test_link_recovery_rearms_the_self_raise_after_ack);
  RUN_TEST(test_ack_during_a_run_routes_to_run_summary);
  RUN_TEST(test_ack_while_idle_routes_home);
  RUN_TEST(test_ack_route_is_captured_at_raise_not_at_ack);
  RUN_TEST(test_ack_is_always_allowed_and_clears);
  RUN_TEST(test_ack_when_clear_is_a_no_op);
  RUN_TEST(test_overtemp_survives_acknowledge);
  RUN_TEST(test_second_episode_after_ack_starts_a_fresh_count);
  RUN_TEST(test_updated_at_ms_bumps_on_every_raise);
  return UNITY_END();
}
