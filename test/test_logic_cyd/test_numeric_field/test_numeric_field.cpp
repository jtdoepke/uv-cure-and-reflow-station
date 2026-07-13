// native_logic_cyd suite — pure host tests of NumericFieldConfig, the shared per-field config
// behind both numeric editors (value-stepper §24 / keypad §26). No LVGL, no Arduino: it is
// plain maths — clamp, step-and-saturate, at-limit, the >20-step editor-routing rule, the
// above-default caution, and value formatting.
#include <unity.h>

#include <cstring>

#include "numeric_field.h"

void setUp(void) {}
void tearDown(void) {}

// Idle timeout: 1–10 min, step 1, default 2 (a real stepper field, §24).
static NumericFieldConfig timeout_cfg() {
  return NumericFieldConfig{1, 10, 1, 2, "min", nullptr};
}

void test_clamp_bounds(void) {
  NumericFieldConfig c = timeout_cfg();
  TEST_ASSERT_EQUAL_INT32(1, c.clamp(-5));
  TEST_ASSERT_EQUAL_INT32(1, c.clamp(1));
  TEST_ASSERT_EQUAL_INT32(6, c.clamp(6));
  TEST_ASSERT_EQUAL_INT32(10, c.clamp(10));
  TEST_ASSERT_EQUAL_INT32(10, c.clamp(99));
}

void test_step_saturates_at_limits(void) {
  NumericFieldConfig c = timeout_cfg();
  TEST_ASSERT_EQUAL_INT32(6, c.stepUp(5));
  TEST_ASSERT_EQUAL_INT32(4, c.stepDown(5));
  TEST_ASSERT_EQUAL_INT32(10, c.stepUp(10)); // already at max, stays
  TEST_ASSERT_EQUAL_INT32(1, c.stepDown(1)); // already at min, stays
}

void test_step_larger_than_one(void) {
  // Brightness bias: 0–100 %, step 5 → 20 steps end to end (exactly the stepper boundary).
  NumericFieldConfig c{0, 100, 5, 50, "%", nullptr};
  TEST_ASSERT_EQUAL_INT32(55, c.stepUp(50));
  TEST_ASSERT_EQUAL_INT32(45, c.stepDown(50));
  TEST_ASSERT_EQUAL_INT32(100, c.stepUp(100));
  TEST_ASSERT_EQUAL_INT32(0, c.stepDown(0));
}

void test_at_limits(void) {
  NumericFieldConfig c = timeout_cfg();
  TEST_ASSERT_TRUE(c.atMin(1));
  TEST_ASSERT_TRUE(c.atMin(-3)); // clamps below min → treated as at-min
  TEST_ASSERT_FALSE(c.atMin(2));
  TEST_ASSERT_TRUE(c.atMax(10));
  TEST_ASSERT_TRUE(c.atMax(50));
  TEST_ASSERT_FALSE(c.atMax(9));
}

void test_uses_stepper_rule_boundary(void) {
  // (max-min)/step <= 20 → stepper; > 20 → keypad.
  TEST_ASSERT_TRUE(timeout_cfg().usesStepper());                          // (10-1)/1 = 9
  TEST_ASSERT_TRUE((NumericFieldConfig{0, 100, 5, 0}.usesStepper()));     // 20 → stepper
  TEST_ASSERT_FALSE((NumericFieldConfig{0, 105, 5, 0}.usesStepper()));    // 21 → keypad
  TEST_ASSERT_FALSE((NumericFieldConfig{60, 250, 1, 100}.usesStepper())); // temp cap → keypad
  TEST_ASSERT_FALSE((NumericFieldConfig{0, 10, 0, 0}.usesStepper())); // degenerate step → keypad
}

void test_caution_only_above_default(void) {
  NumericFieldConfig c{1, 10, 1, 2, "min", "Above default"};
  TEST_ASSERT_FALSE(c.cautionActive(2)); // at default
  TEST_ASSERT_FALSE(c.cautionActive(1)); // below default
  TEST_ASSERT_TRUE(c.cautionActive(3));  // above default
  // No caution string configured → never active.
  NumericFieldConfig none = timeout_cfg();
  TEST_ASSERT_FALSE(none.cautionActive(10));
}

void test_format_with_and_without_units(void) {
  char buf[24];
  timeout_cfg().format(2, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("2 min", buf);

  NumericFieldConfig no_units{0, 9, 1, 0, "", nullptr};
  no_units.format(7, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("7", buf);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_clamp_bounds);
  RUN_TEST(test_step_saturates_at_limits);
  RUN_TEST(test_step_larger_than_one);
  RUN_TEST(test_at_limits);
  RUN_TEST(test_uses_stepper_rule_boundary);
  RUN_TEST(test_caution_only_above_default);
  RUN_TEST(test_format_with_and_without_units);
  return UNITY_END();
}
