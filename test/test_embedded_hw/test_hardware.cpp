// test_embedded_hw suite — runs on a real CYD via `pio test -e embedded`. Flash over the
// Micro-USB port. Asserts LovyanGFX bring-up, rotated geometry, brightness, heap headroom, and a
// human-in-the-loop touch target (you tap a drawn box).
//
// Board-agnostic on purpose: everything panel-specific comes from cyd_board.h / panel.h, so the
// env's [board_*] flags decide which board this is flashed to.
#include <Arduino.h>
#include <unity.h>

#include "cyd_board.h"

static LGFX gfx;

void setUp(void) {}
void tearDown(void) {}

// LovyanGFX init() returns bool — a real assertion, not just "didn't crash".
void test_gfx_init(void) {
  TEST_ASSERT_TRUE(gfx.init());
}

// The one cross-check the static_asserts cannot do: that the PANEL_* flags the whole UI lays
// itself out against match what the panel driver actually reports at runtime.
void test_rotated_dimensions(void) {
  gfx.setRotation(kRotation);
  TEST_ASSERT_EQUAL_INT(panel::W, gfx.width());
  TEST_ASSERT_EQUAL_INT(panel::H, gfx.height());
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
  // Centred, and sized as the design guide's 10 mm touch floor rather than a px literal: the box
  // then lands on-screen and stays the same physical size on any panel.
  const int bw = panel::pxFromMmX10(100), bh = bw;
  const int bx = (panel::W - bw) / 2, by = (panel::H - bh) / 2;
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
  // Tolerance = half the box, i.e. "the tap landed in the box you were told to tap". Derived
  // rather than a px literal so it means the same thing on a panel with a different pitch.
  TEST_ASSERT_INT_WITHIN(bw / 2, bx + bw / 2, x);
  TEST_ASSERT_INT_WITHIN(bh / 2, by + bh / 2, y);
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
