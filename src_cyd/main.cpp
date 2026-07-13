// ESP32-2432S028 "Cheap Yellow Display" (dual-USB v3 / ST7789) demo.
//
// LovyanGFX drives the panel + touch; display config is in include/LGFX_CYD2USB.hpp.
// The app runs a startup color self-test, then shows a "Hello CYD!" label and a
// touch-counting button. See CLAUDE.md for build/upload commands.

#include <Arduino.h>
#include <lvgl.h>
#include "LGFX_CYD2USB.hpp"
#include "auto_brightness.h"      // ambient-light -> backlight logic (lib/app_logic, §18)
#include "esp32_ambient_light.h"  // LDR IAmbientLight adapter (firmware glue)
#include "home_screen.h"          // UI construction lives in lib/ui_logic (host-testable)
#include "lgfx_backlight.h"       // LGFX IBacklight adapter (firmware glue)
#include "nvs_settings_storage.h" // NVS-backed ISettingsStorage adapter (firmware glue)
#include "settings_screen.h"      // Settings hub + panels (§24)
#include "settings_store.h"       // typed device settings (lib/app_logic)
#include "sleep_controller.h"     // idle sleep/wake policy (lib/app_logic, §17)
#include "subjects.h"
#include "schema.h" // shared wire-contract identity (lib/protocol)
#if defined(UI_DEV_TOOLS)
#include "ui_dev_tools.h" // WiFi screenshot/touch API (esp32dev_cyd_uidev env only)
#endif

static const uint16_t SCR_W = 320, SCR_H = 240; // landscape
static LGFX gfx;

// Double-buffered async DMA flush. Set to 0 to reclaim ~15 KB DRAM: that drops the
// second draw buffer and reverts to the original single-buffer, CPU-blocking flush.
// Turn this off FIRST if the WiFi dev build (esp32dev_cyd_uidev) starts failing the
// draw-buffer malloc — the 2nd buffer is the easiest ~15 KB to give back, at the
// cost of display responsiveness.
#define DISP_DOUBLE_BUFFER 1

// 1/10-screen partial draw buffer, RGB565 (2 bytes/pixel). This board has no PSRAM,
// so never allocate a full-frame buffer. Heap-allocated in setup() rather than static:
// the WiFi stack in the esp32dev_cyd_uidev env overflows the static DRAM segment otherwise.
static constexpr size_t DRAW_BUF_BYTES = SCR_W * SCR_H / 10 * 2;
static uint8_t *draw_buf = nullptr;
#if DISP_DOUBLE_BUFFER
static uint8_t *draw_buf2 = nullptr; // second buffer: render next chunk while this one DMAs out
#endif
static uint32_t last_tick = 0;

// Device settings, persisted in NVS. Loaded at boot; the Settings screen (§24) edits them.
static NvsSettingsStorage g_settings_storage;
static SettingsStore g_settings(g_settings_storage);
static SettingsScreen g_settings_screen;

// Auto-brightness (§18) + idle sleep/wake (§17). AutoBrightness owns the backlight; the
// SleepController decides awake/asleep and AutoBrightness ramps the backlight off/on to match.
// Fed from g_settings each loop (auto on/off, brightness bias, idle timeout).
static Esp32AmbientLight g_ambient;
static LgfxBacklight g_backlight(gfx);
static AutoBrightness g_auto_brightness(g_ambient, g_backlight);
static SleepController g_sleep;

static void build_home();

// Home ‹ Settings navigation. There is no global screen manager yet (C4/C6); Home publishes a
// NAV_SETTINGS intent on its Settings tile, this observer swaps the active screen to the Settings
// hub, and the hub's Back returns here. The observer lives on the subject (not a widget), so it
// survives each screen rebuild.
static void on_settings_exit(void *) {
  build_home();
}

static void on_nav_request(lv_observer_t *, lv_subject_t *subject) {
  if (lv_subject_get_int(subject) == NAV_SETTINGS) {
    lv_obj_clean(lv_screen_active());
    g_settings_screen.setExitHandler(on_settings_exit, nullptr);
    g_settings_screen.begin(lv_screen_active(), g_settings);
  }
}

static void build_home() {
  lv_subject_set_int(&subj_nav_request, NAV_NONE); // reset so re-tapping Settings re-triggers
  lv_obj_clean(lv_screen_active());
  create_home_screen(lv_screen_active());
}

// Wake the display the instant the machine leaves idle — a HOT chamber, a run start, or a
// fault must never be hidden behind a dark screen (§17/§22). The loop's sleep tick also keeps
// us awake while non-idle; this observer just makes the wake immediate on the state change.
static void on_run_state(lv_observer_t *, lv_subject_t *subject) {
  if (lv_subject_get_int(subject) != RUN_IDLE) {
    g_sleep.noteActivity(millis());
    // TODO(§17): door-open wake once the controller reports door state (subj_door_open) — the
    // link/telemetry decode that would drive it lands with the controller-link integration.
  }
}

static void my_disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  int32_t w = area->x2 - area->x1 + 1;
  int32_t h = area->y2 - area->y1 + 1;
#if DISP_DOUBLE_BUFFER
  // Async DMA: hold one write session open across the frame's chunks and push via
  // DMA without waiting. With two buffers LVGL renders the next chunk while this one
  // DMAs out. LVGL renders RGB565_SWAPPED (set in setup), so feeding swap565_t hits
  // LovyanGFX's zero-copy DMA path instead of a CPU byte-swap. Buffer reuse is safe
  // because the next push (or this endWrite) blocks on the prior DMA before touching
  // the buffer again.
  if (gfx.getStartCount() == 0) {
    gfx.startWrite();
  }
  gfx.pushImageDMA(area->x1, area->y1, w, h, reinterpret_cast<lgfx::swap565_t *>(px_map));
  if (lv_display_flush_is_last(disp)) {
    gfx.endWrite(); // waits out the final DMA, closes the session
  }
  lv_display_flush_ready(disp);
#else
  // Original single-buffer synchronous path: CPU-driven SPI with a runtime RGB565
  // byte-swap (the `true`). Blocks until the whole area is pushed.
  gfx.startWrite();
  gfx.setAddrWindow(area->x1, area->y1, w, h);
  gfx.pushPixels((uint16_t *)px_map, w * h, true);
  gfx.endWrite();
  lv_display_flush_ready(disp);
#endif
}

static void my_touch_read(lv_indev_t *indev, lv_indev_data_t *data) {
#if defined(UI_DEV_TOOLS)
  int16_t sx, sy;
  if (ui_dev_touch_get(&sx, &sy)) { // injected touch takes precedence over the panel
    g_sleep.noteActivity(millis()); // keep the screen awake during WiFi dev-shot/touch sessions
    data->point.x = sx;
    data->point.y = sy;
    data->state = LV_INDEV_STATE_PRESSED;
    return;
  }
#endif
  uint16_t x, y;
  if (gfx.getTouch(&x, &y)) { // getTouch() returns calibrated screen coords
    // A touch always counts as activity. If it woke the screen, consume it: the wake tap lights
    // the display without also actuating the control beneath it (§17).
    bool wasAsleep = !g_sleep.awake();
    g_sleep.noteActivity(millis());
    if (wasAsleep) {
      data->state = LV_INDEV_STATE_RELEASED;
      return;
    }
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
#if !defined(UI_DEV_TOOLS) // skipped in the dev env: saves 3.2 s per flash-iterate cycle
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
    lv_refr_now(nullptr);
    delay(800);
  }
  lv_obj_delete(lbl);
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), LV_PART_MAIN);
}
#endif // !UI_DEV_TOOLS

void setup() {
  Serial.begin(115200);
  // Two %08lx halves: 32-bit printf has no portable 64-bit format here.
  Serial.printf("[protocol] ver=%u schema=%08lx%08lx\n", (unsigned)protocol::kProtoVer,
                (unsigned long)(protocol::kSchemaHash >> 32),
                (unsigned long)(protocol::kSchemaHash & 0xFFFFFFFFu));

  gfx.init();
  gfx.setRotation(1);     // landscape
  gfx.setBrightness(255); // full for the boot color self-test; AutoBrightness takes over in loop()
  analogSetPinAttenuation(Esp32AmbientLight::kPin, ADC_11db); // full ~0-3.3 V LDR swing (§18)

  lv_init();

  draw_buf = static_cast<uint8_t *>(malloc(DRAW_BUF_BYTES)); // internal DRAM (no PSRAM)
#if DISP_DOUBLE_BUFFER
  draw_buf2 = static_cast<uint8_t *>(malloc(DRAW_BUF_BYTES));
  if (draw_buf == nullptr || draw_buf2 == nullptr) {
#else
  if (draw_buf == nullptr) {
#endif
    Serial.println("FATAL: LVGL draw buffer allocation failed");
    abort();
  }

  lv_display_t *disp = lv_display_create(SCR_W, SCR_H);
  lv_display_set_flush_cb(disp, my_disp_flush);
#if DISP_DOUBLE_BUFFER
  // RGB565_SWAPPED makes LVGL render in the panel's byte order, so the flush can feed
  // swap565_t straight to DMA with no conversion (see my_disp_flush). Revert this and
  // the buffer line below together if DISP_DOUBLE_BUFFER is turned off.
  lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
  lv_display_set_buffers(disp, draw_buf, draw_buf2, DRAW_BUF_BYTES, LV_DISPLAY_RENDER_MODE_PARTIAL);
#else
  lv_display_set_buffers(disp, draw_buf, nullptr, DRAW_BUF_BYTES, LV_DISPLAY_RENDER_MODE_PARTIAL);
#endif

  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, my_touch_read);

#if !defined(UI_DEV_TOOLS)
  run_display_test();
#endif

  // The subjects boot to safe defaults (idle, no-link); real telemetry/handshake wiring that
  // drives them lands with the controller-link integration.
  ui_subjects_init();

  // Load persisted settings and clamp caps to the current hard-max (§4/§24), then publish the
  // cross-screen values so any consumer sees them before Settings is opened.
  g_settings.load();
  settings_publish_subjects(g_settings);

  create_home_screen(lv_screen_active());
  // Watch for the Home → Settings navigation intent (persists across screen rebuilds).
  lv_subject_add_observer(&subj_nav_request, on_nav_request, nullptr);
  // Wake immediately on any non-idle machine state (HOT / running / fault) — §17/§22.
  lv_subject_add_observer(&subj_run_state, on_run_state, nullptr);

#if defined(UI_DEV_TOOLS)
  ui_dev_tools_begin(gfx);
#endif
}

void loop() {
  auto now = millis();
  lv_tick_inc(now - last_tick);
  last_tick = now;
  lv_timer_handler();

  // Sleep/wake (§17) + auto-brightness (§18). Sleep only when idle AND cool — the run-state
  // subject already folds HOT/running/fault into non-idle states, so that is a single predicate.
  // Never sleep during a run: the Run screen keeps the machine non-idle, and a dark screen would
  // stop the heartbeat -> controller aborts to safe (§9).
  bool sleep_allowed = (lv_subject_get_int(&subj_run_state) == RUN_IDLE);
  g_sleep.setIdleTimeoutMs(static_cast<uint32_t>(g_settings.idleTimeoutMin()) * 60000U);
  g_sleep.tick(now, sleep_allowed);
  g_auto_brightness.setAwake(g_sleep.awake());
  g_auto_brightness.setEnabled(g_settings.autoBrightness());
  // Brightness bias: normally the stored value, but while the bias stepper is open, preview the
  // in-progress value live so the screen brightens/dims as you dial it (§18/§24).
  int32_t bias = g_settings_screen.isEditingBrightnessBias()
                     ? g_settings_screen.liveBrightnessBias()
                     : g_settings.brightnessBias();
  g_auto_brightness.setBias(bias);
  g_auto_brightness.tick(now);

  // On-glass curve tuning trace (1 Hz): the raw LDR (GPIO34) vs the resulting backlight. On this
  // board raw is ~0 in room light and climbs into the hundreds/thousands only as it gets dark
  // (§18). Handy while tuning AutoBrightness::kCurve; safe to drop once the curve is dialed in.
  static uint32_t last_ldr_log = 0;
  if (static_cast<uint32_t>(now - last_ldr_log) >= 1000U) {
    last_ldr_log = now;
    Serial.printf("[ldr] raw=%4d backlight=%3u auto=%d bias=%ld awake=%d\n", analogRead(34),
                  g_auto_brightness.level(), static_cast<int>(g_settings.autoBrightness()),
                  static_cast<long>(bias), static_cast<int>(g_sleep.awake()));
  }

#if defined(UI_DEV_TOOLS)
  ui_dev_tools_loop();
#endif
  delay(5);
}
