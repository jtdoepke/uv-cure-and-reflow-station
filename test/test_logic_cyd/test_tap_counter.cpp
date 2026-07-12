// native_logic_cyd suite — pure host tests of TapCounter behind the ITouch port.
// No LovyanGFX, no LVGL, no Arduino: a FakeTouch is injected and driven directly.
#include <unity.h>

#include "tap_counter.h"
#include "helpers/fake_touch.h"

static FakeTouch touch;

void setUp(void) {
  touch = FakeTouch();
} // reset the double before each test
void tearDown(void) {}

void test_starts_at_zero(void) {
  TapCounter c(touch);
  TEST_ASSERT_EQUAL_INT(0, c.count());
}

void test_counts_one_press_and_records_coords(void) {
  TapCounter c(touch);
  touch.nextTouch = true;
  touch.tx = 42;
  touch.ty = 99;
  TEST_ASSERT_TRUE(c.poll()); // rising edge
  TEST_ASSERT_EQUAL_INT(1, c.count());
  TEST_ASSERT_EQUAL_INT(42, c.last_x());
  TEST_ASSERT_EQUAL_INT(99, c.last_y());
}

void test_held_press_counts_once(void) {
  TapCounter c(touch);
  touch.nextTouch = true;
  c.poll();
  TEST_ASSERT_FALSE(c.poll()); // still held -> no new edge
  c.poll();
  TEST_ASSERT_EQUAL_INT(1, c.count());
}

void test_release_then_press_counts_again(void) {
  TapCounter c(touch);
  touch.nextTouch = true;
  c.poll();
  touch.nextTouch = false;
  c.poll();
  touch.nextTouch = true;
  c.poll();
  TEST_ASSERT_EQUAL_INT(2, c.count());
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_starts_at_zero);
  RUN_TEST(test_counts_one_press_and_records_coords);
  RUN_TEST(test_held_press_counts_once);
  RUN_TEST(test_release_then_press_counts_again);
  return UNITY_END();
}
