// ESP32-2432S028 "Cheap Yellow Display" (dual-USB v3 / ST7789) demo.
//
// LovyanGFX drives the panel + touch; display config is in include/LGFX_CYD2USB.hpp.
// The app runs a startup color self-test, then shows a "Hello CYD!" label and a
// touch-counting button. See CLAUDE.md for build/upload commands.

#include <Arduino.h>
#include <lvgl.h>
#include "LGFX_CYD2USB.hpp"
#include "main_ui.h" // UI construction lives in lib/ui_logic (host-testable)

static const uint16_t SCR_W = 320, SCR_H = 240; // landscape
static LGFX gfx;

// 1/10-screen partial draw buffer, RGB565 (2 bytes/pixel). This board has no PSRAM,
// so never allocate a full-frame buffer.
static uint8_t draw_buf[SCR_W * SCR_H / 10 * 2];
static uint32_t last_tick = 0;

static void my_disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;
  gfx.startWrite();
  gfx.setAddrWindow(area->x1, area->y1, w, h);
  gfx.pushPixels((uint16_t *)px_map, w * h, true);
  gfx.endWrite();
  lv_display_flush_ready(disp);
}

static void my_touch_read(lv_indev_t *indev, lv_indev_data_t *data) {
  uint16_t x, y;
  if (gfx.getTouch(&x, &y)) { // getTouch() returns calibrated screen coords
    data->point.x = x;
    data->point.y = y;
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

// Startup self-test: fill the whole screen with each named color for ~0.8s.
// Confirms the panel works and colors are right BEFORE the UI loads:
//   - a color shown as its photo-negative -> flip cfg.invert in LGFX_CYD2USB.hpp
//   - RED renders as blue / BLUE as red    -> flip cfg.rgb_order
static void run_display_test() {
  lv_obj_t *scr = lv_screen_active();
  const uint32_t colors[] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF};
  const char *names[] = {"RED", "GREEN", "BLUE", "WHITE"};
  lv_obj_t *lbl = lv_label_create(scr);
  lv_obj_center(lbl);
  for (int i = 0; i < 4; i++) {
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors[i]), LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(i == 3 ? 0x000000 : 0xFFFFFF), 0);
    lv_label_set_text(lbl, names[i]);
    lv_refr_now(NULL);
    delay(800);
  }
  lv_obj_delete(lbl);
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), LV_PART_MAIN);
}

void setup() {
  Serial.begin(115200);

  gfx.init();
  gfx.setRotation(1); // landscape
  gfx.setBrightness(255);

  lv_init();

  lv_display_t *disp = lv_display_create(SCR_W, SCR_H);
  lv_display_set_flush_cb(disp, my_disp_flush);
  lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);

  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, my_touch_read);

  run_display_test();

  create_main_ui(lv_screen_active());
}

void loop() {
  auto now = millis();
  lv_tick_inc(now - last_tick);
  last_tick = now;
  lv_timer_handler();
  delay(5);
}
