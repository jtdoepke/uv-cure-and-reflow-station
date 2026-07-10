// test_embedded_hw suite — runs on the real ESP32-2432S028 via `pio test -e embedded`.
// Flash over the Micro-USB port. Asserts LovyanGFX bring-up, rotated geometry, brightness,
// heap headroom, and a human-in-the-loop touch target (you tap a drawn box).
#include <Arduino.h>
#include <unity.h>

#include "LGFX_CYD2USB.hpp"

static LGFX gfx;

void setUp(void) {}
void tearDown(void) {}

// LovyanGFX init() returns bool — a real assertion, not just "didn't crash".
void test_gfx_init(void) {
  TEST_ASSERT_TRUE(gfx.init());
}

void test_rotated_dimensions(void) {
  gfx.setRotation(1); // landscape
  TEST_ASSERT_EQUAL_INT(320, gfx.width());
  TEST_ASSERT_EQUAL_INT(240, gfx.height());
}

void test_brightness(void) {
  gfx.setBrightness(200);
  TEST_PASS(); // no crash / no hang
}

void test_heap_headroom(void) {
  TEST_ASSERT_GREATER_THAN(50000, ESP.getFreeHeap());
}

// Visual output can't be auto-asserted, so verify touch with a human in the loop: draw a
// red box and assert the tap lands inside it. getTouch() returns calibrated screen coords.
void test_touch_target(void) {
  gfx.fillScreen(0x0000);
  const int bx = 140, by = 100, bw = 40, bh = 40;
  gfx.fillRect(bx, by, bw, bh, 0xF800); // red
  Serial.println(">> Tap the RED box within 10 s");

  uint16_t x = 0, y = 0;
  uint32_t t0 = millis();
  bool got = false;
  while (millis() - t0 < 10000) {
    if (gfx.getTouch(&x, &y)) {
      got = true;
      break;
    }
    delay(10);
  }
  TEST_ASSERT_TRUE_MESSAGE(got, "no touch detected within timeout");
  TEST_ASSERT_INT_WITHIN(30, bx + bw / 2, x); // within ~30 px of box center
  TEST_ASSERT_INT_WITHIN(30, by + bh / 2, y);
}

void setup() {
  delay(2000); // mandatory: let the host reconnect to serial after the post-upload reset
  UNITY_BEGIN();
  RUN_TEST(test_gfx_init);
  RUN_TEST(test_rotated_dimensions);
  RUN_TEST(test_brightness);
  RUN_TEST(test_heap_headroom);
  RUN_TEST(test_touch_target);
  UNITY_END();
}

void loop() {}
