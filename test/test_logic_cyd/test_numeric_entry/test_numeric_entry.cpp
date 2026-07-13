// native_logic_cyd suite — pure host tests of NumericEntry, the digit-accumulation logic behind
// the on-screen keypad (§26, C1). No LVGL, no Arduino: append/backspace/clear, the over-max
// block, empty-vs-typed-0, pre-load, and the okEnabled (in-range) rule that gates OK.
#include <unity.h>

#include <cstring>

#include "numeric_entry.h"

void setUp(void) {}
void tearDown(void) {}

// A wide-range field that fails the >20-step rule → keypad (§24/§26). UV temp cap: 60–250 °C.
static NumericFieldConfig cap_cfg() {
  return NumericFieldConfig{60, 250, 1, 100, "°C", nullptr};
}

void test_reset_preloads_current_value(void) {
  NumericEntry e;
  e.reset(cap_cfg(), 100);
  TEST_ASSERT_FALSE(e.isEmpty());
  TEST_ASSERT_EQUAL_INT32(100, e.value());
  TEST_ASSERT_TRUE(e.okEnabled());
}

void test_reset_clamps_initial_into_range(void) {
  NumericEntry e;
  e.reset(cap_cfg(), 9999);
  TEST_ASSERT_EQUAL_INT32(250, e.value()); // clamped to max
  e.reset(cap_cfg(), 0);
  TEST_ASSERT_EQUAL_INT32(60, e.value()); // clamped to min
}

void test_append_builds_the_value(void) {
  NumericEntry e;
  e.reset(cap_cfg(), 100);
  e.clear(); // start empty
  TEST_ASSERT_TRUE(e.isEmpty());
  TEST_ASSERT_TRUE(e.appendDigit(2));
  TEST_ASSERT_TRUE(e.appendDigit(4));
  TEST_ASSERT_TRUE(e.appendDigit(5));
  TEST_ASSERT_EQUAL_INT32(245, e.value());
  TEST_ASSERT_FALSE(e.isEmpty());
}

void test_append_blocks_over_max(void) {
  NumericEntry e;
  e.reset(cap_cfg(), 100);
  e.clear();
  TEST_ASSERT_TRUE(e.appendDigit(2));
  TEST_ASSERT_TRUE(e.appendDigit(5));
  // 25 * 10 + 5 = 255 > 250 → refused, value unchanged at 25.
  TEST_ASSERT_FALSE(e.appendDigit(5));
  TEST_ASSERT_EQUAL_INT32(25, e.value());
  // A digit that keeps it in range is still accepted: 25 -> 250.
  TEST_ASSERT_TRUE(e.appendDigit(0));
  TEST_ASSERT_EQUAL_INT32(250, e.value());
}

void test_append_ignores_out_of_0_9(void) {
  NumericEntry e;
  e.reset(cap_cfg(), 100);
  e.clear();
  TEST_ASSERT_FALSE(e.appendDigit(-1));
  TEST_ASSERT_FALSE(e.appendDigit(10));
  TEST_ASSERT_TRUE(e.isEmpty());
}

void test_backspace_walks_back_and_empties(void) {
  NumericEntry e;
  e.reset(cap_cfg(), 100);
  e.clear();
  e.appendDigit(2);
  e.appendDigit(4);
  e.appendDigit(5); // 245
  e.backspace();
  TEST_ASSERT_EQUAL_INT32(24, e.value());
  e.backspace();
  TEST_ASSERT_EQUAL_INT32(2, e.value());
  e.backspace();
  TEST_ASSERT_TRUE(e.isEmpty()); // last digit gone → empty, not typed-0
  TEST_ASSERT_EQUAL_INT32(0, e.value());
  e.backspace(); // no-op when already empty
  TEST_ASSERT_TRUE(e.isEmpty());
}

void test_backspace_from_preloaded_value(void) {
  NumericEntry e;
  e.reset(cap_cfg(), 245); // three "entered" digits
  e.backspace();
  TEST_ASSERT_EQUAL_INT32(24, e.value());
  TEST_ASSERT_FALSE(e.isEmpty());
}

void test_typed_zero_is_not_empty(void) {
  // A field whose range admits 0 (e.g. brightness bias 0–100 %, but wide enough to keypad).
  NumericEntry e;
  e.reset(NumericFieldConfig{0, 200, 1, 0, "%", nullptr}, 50);
  e.clear();
  TEST_ASSERT_TRUE(e.isEmpty());
  TEST_ASSERT_TRUE(e.appendDigit(0));
  TEST_ASSERT_FALSE(e.isEmpty()); // typed "0" is content
  TEST_ASSERT_EQUAL_INT32(0, e.value());
  TEST_ASSERT_TRUE(e.okEnabled()); // 0 is in [0,200]
}

void test_ok_enabled_boundaries(void) {
  NumericEntry e;
  e.reset(cap_cfg(), 100);
  e.clear();
  TEST_ASSERT_FALSE(e.okEnabled()); // empty → disabled
  e.appendDigit(6);
  TEST_ASSERT_FALSE(e.okEnabled()); // 6 < min 60 → disabled (incomplete-low)
  e.appendDigit(0);                 // 60 == min
  TEST_ASSERT_TRUE(e.okEnabled());
  e.clear();
  e.appendDigit(2);
  e.appendDigit(5);
  e.appendDigit(0); // 250 == max
  TEST_ASSERT_TRUE(e.okEnabled());
}

void test_format_empty_and_value(void) {
  char buf[24];
  NumericEntry e;
  e.reset(cap_cfg(), 100);
  e.clear();
  e.format(buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("", buf); // empty renders blank, not "0 °C"
  e.appendDigit(2);
  e.appendDigit(4);
  e.appendDigit(5);
  e.format(buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("245 \xC2\xB0"
                           "C",
                           buf); // "245 °C" (° is UTF-8 0xC2 0xB0)
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_reset_preloads_current_value);
  RUN_TEST(test_reset_clamps_initial_into_range);
  RUN_TEST(test_append_builds_the_value);
  RUN_TEST(test_append_blocks_over_max);
  RUN_TEST(test_append_ignores_out_of_0_9);
  RUN_TEST(test_backspace_walks_back_and_empties);
  RUN_TEST(test_backspace_from_preloaded_value);
  RUN_TEST(test_typed_zero_is_not_empty);
  RUN_TEST(test_ok_enabled_boundaries);
  RUN_TEST(test_format_empty_and_value);
  return UNITY_END();
}
