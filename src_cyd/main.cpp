// CYD "Cheap Yellow Display" HMI firmware.
//
// LovyanGFX drives the panel + touch. Which board, which orientation, and every pin this file
// needs come from include/cyd_board.h — deliberately, so this stays the app's composition root
// and not a board definition. The app runs a startup color self-test, then shows the Home screen.
// See CLAUDE.md for build/upload commands.

#include <Arduino.h>
#include <lvgl.h>
#include "auto_brightness.h"        // ambient-light -> backlight logic (lib/app_logic, §18)
#include "cyd_board.h"              // this board's pins, capabilities, orientation, buffers
#include "cyd_link.h"               // reliability facade (lib/protocol, §9)
#include "esp32_ambient_light.h"    // LDR IAmbientLight adapter (firmware glue)
#include "esp32_clock.h"            // IClock adapter for the link's cadences (firmware glue)
#include "esp32_serial_transport.h" // ISerialTransport adapter over the link UART (firmware glue)
#include "frame_link.h"             // TinyFrame framing (lib/protocol)
#include "home_screen.h"            // UI construction lives in lib/ui_logic (host-testable)
#include "home_viewmodel.h"         // handshake -> LinkState mapper (lib/ui_logic)
#include "lgfx_backlight.h"         // LGFX IBacklight adapter (firmware glue)
#include "message_router.h"         // frame -> typed message dispatch (lib/protocol)
#include "nvs_settings_storage.h"   // NVS-backed ISettingsStorage adapter (firmware glue)
#include "settings_screen.h"        // Settings hub + panels (§24)
#include "settings_store.h"         // typed device settings (lib/app_logic)
#include "sleep_controller.h"       // idle sleep/wake policy (lib/app_logic, §17)
#include "subjects.h"
#include "schema.h" // shared wire-contract identity (lib/protocol)
#if defined(UI_DEV_TOOLS)
#include "ui_dev_tools.h" // WiFi screenshot/touch API (esp32dev_cyd_uidev env only)
#endif

static LGFX gfx;

// Partial draw buffers, RGB565 — sized and explained in cyd_board.h. Heap-allocated in setup()
// rather than static: the WiFi stack in the esp32dev_cyd_uidev env overflows the static DRAM
// segment otherwise.
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
static Esp32AmbientLight g_ambient(kAmbientPin);
static LgfxBacklight g_backlight(gfx);
static AutoBrightness g_auto_brightness(g_ambient, g_backlight);
static SleepController g_sleep;

// CYD <-> controller link (§9). The UART, its pins and its buffer sizing live in cyd_board.h —
// §2 put the link clear of the CYD's own UART0/USB, so the serial monitor and flashing stay free
// of contention and this side needs no bench flag (the controller's does; see
// src_control/control_board.h).
//
// This is production wiring, not bench scaffolding: it is safe by construction because
// HeartbeatSender boots at session=0/enable=false and the controller's SessionGate filters any
// session it has not adopted — so an un-started CYD authorizes exactly nothing. Hello +
// handshake + inert heartbeats is what production should do before C6's Setup/Confirm (§19)
// starts a run. The MessageRouter is default-constructed and bound in setup(): FrameLink binds
// its handler at construction but CydLink needs the FrameLink to send, so the cycle is broken
// with setObserver().
static Esp32Clock g_link_clk;
static Esp32SerialTransport g_link_uart(linkSerial());
static protocol::MessageRouter g_link_router;
static protocol::FrameLink g_link(g_link_uart, TF_MASTER, g_link_router); // CYD = TF_MASTER
static protocol::CydLink g_cyd_link(g_link, g_link_clk);

#if defined(CYD_BENCH_LINK)
// A8 bench stimulus (§8 step 1) — NOT production behavior; §19/C6's Setup/Confirm owns starting
// a run. Drives the controller to authorized() at boot so the dummy-load LEDs light and the
// fail-safe cut is observable when the TX is pulled.
//
// Recipe -> Ack -> Start mirrors the real flow (a run always uploads a recipe before starting)
// and exercises both setup-path commands rather than just one. The session is random per boot
// so a CYD reboot presents a genuinely new session, as §9 says it must.
static uint32_t g_bench_session = 0;
static bool g_bench_recipe_sent = false;
static bool g_bench_start_sent = false;

static void bench_link_stimulus() {
  using protocol::ReliableSender;
  if (!g_cyd_link.handshake().matched()) {
    return; // schema gate first, fail-closed (§9)
  }
  auto &tx = g_cyd_link.sender();

  if (!g_bench_recipe_sent) {
    if (tx.state() == ReliableSender::State::Idle) {
      oven_Recipe rec = oven_Recipe_init_default;
      rec.id = 1;
      rec.mode = oven_Mode_MODE_CURE;
      rec.segments_count = 1;
      rec.segments[0].dur_ms = 60000;
      rec.segments[0].heat_c = 80.0F;
      rec.segments[0].interp = oven_Interp_INTERP_HOLD;
      g_bench_recipe_sent = tx.sendRecipe(rec); // the sender stamps the seq
    }
    return;
  }
  if (!g_bench_start_sent) {
    if (tx.state() == ReliableSender::State::Acked) {
      oven_Start st = oven_Start_init_default;
      st.session = g_bench_session;
      st.recipe_id = 1;
      g_bench_start_sent = tx.sendStart(st);
    }
    return;
  }
  // Start acked -> the controller has adopted the session, so heartbeats can authorize it.
  if (tx.state() == ReliableSender::State::Acked &&
      g_cyd_link.heartbeat().session() != g_bench_session) {
    g_cyd_link.heartbeat().setSession(g_bench_session);
    g_cyd_link.heartbeat().setEnable(true); // the explicit HEAT_EN bit (§9)
  }
}
#endif // CYD_BENCH_LINK

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
//   - a color shown as its photo-negative -> flip cfg.invert in this board's LGFX header
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
  gfx.setRotation(kRotation); // the board's mounting orientation (cyd_board.h)
  gfx.setBrightness(255); // full for the boot color self-test; AutoBrightness takes over in loop()
  if constexpr (kHasAmbientLight) {
    analogSetPinAttenuation(kAmbientPin, kAmbientAtten);
  }

  lv_init();

  draw_buf = static_cast<uint8_t *>(malloc(kDrawBufBytes)); // internal DRAM (no PSRAM)
#if DISP_DOUBLE_BUFFER
  draw_buf2 = static_cast<uint8_t *>(malloc(kDrawBufBytes));
  if (draw_buf == nullptr || draw_buf2 == nullptr) {
#else
  if (draw_buf == nullptr) {
#endif
    Serial.println("FATAL: LVGL draw buffer allocation failed");
    abort();
  }

  lv_display_t *disp = lv_display_create(panel::W, panel::H);
  lv_display_set_flush_cb(disp, my_disp_flush);
#if DISP_DOUBLE_BUFFER
  // RGB565_SWAPPED makes LVGL render in the panel's byte order, so the flush can feed
  // swap565_t straight to DMA with no conversion (see my_disp_flush). Revert this and
  // the buffer line below together if DISP_DOUBLE_BUFFER is turned off.
  lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
  lv_display_set_buffers(disp, draw_buf, draw_buf2, kDrawBufBytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
#else
  lv_display_set_buffers(disp, draw_buf, nullptr, kDrawBufBytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
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

  // Link last: run_display_test() above spends ~3 s in delay() loops, and there is no point
  // servicing a handshake nothing is pumping. (Hello retransmits would survive it; starting
  // afterwards is simply honest about when we can actually talk.)
  linkSerial().setRxBufferSize(kLinkRxBuf); // both must precede begin()
  linkSerial().setTxBufferSize(kLinkTxBuf);
  linkSerial().begin(kLinkBaud, SERIAL_8N1, kLinkRxPin, kLinkTxPin);
  // Seed the setup-path seq from a per-boot random base. seq is monotonic only *within* a boot,
  // but the controller's dedup treats it as globally unique, so an unseeded reboot would replay
  // seq 1 and have its first Start mistaken for a duplicate — Acked, but the session silently
  // never adopted (see ReliableSender::setSeqBase).
  g_cyd_link.sender().setSeqBase(esp_random());
  g_link_router.setObserver(g_cyd_link); // without this every frame is dropped, silently
  // Hello; service() retransmits until the controller answers. The nonce must be fresh every
  // boot: it is how the controller spots that we restarted and re-announces itself, so the link
  // recovers instead of sitting unmatched (§9 re-sync).
  g_cyd_link.begin(esp_random());
#if defined(CYD_BENCH_LINK)
  g_bench_session = esp_random() | 1U; // non-zero: 0 means "no session" (§9)
  Serial.printf("[bench] link stimulus armed, session=%08lx\n",
                static_cast<unsigned long>(g_bench_session));
#endif

#if defined(UI_DEV_TOOLS)
  ui_dev_tools_begin(gfx);
#endif
}

void loop() {
  auto now = millis();

  // Link first (§9): the controller's 750 ms command-timeout is budgeted against this loop's
  // period, and the LVGL work below is bursty — a screen rebuild can run tens of ms. A dropped
  // heartbeat self-heals on the next tick, so the normal ~5-20 ms period leaves ~10x margin on
  // the 200 ms cadence; sustained blocking past 750 ms would be a real bug worth catching, and
  // §7/B8·2's dedicated link task is the answer if it ever gets close.
  g_link.poll();
  g_cyd_link.service(); // handshake + heartbeat cadence + setup-path retries
  static uint32_t last_link_tick = 0;
  if (static_cast<uint32_t>(now - last_link_tick) >= kLinkTickMs) {
    last_link_tick = now;
    g_link.tick();
  }
#if defined(CYD_BENCH_LINK)
  bench_link_stimulus();
#endif

  // Publish the §9 link health as Home's indicator + run-flow gate (§14). linkAlive() is the
  // part that decays — the controller's telemetry arriving — since the handshake latches and
  // would otherwise keep claiming a link over an unplugged cable. Set every loop rather than
  // diffed: lv_subject_set_int notifies only when the value actually changes
  // (lv_subject_notify_if_changed), so this costs a compare and rebuilds nothing.
  lv_subject_set_int(&subj_link_state,
                     HomeViewModel::linkStateFrom(g_cyd_link.handshake().sawPeer(),
                                                  g_cyd_link.handshake().matched(),
                                                  g_cyd_link.linkAlive()));

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

  // On-glass curve tuning trace (1 Hz): the raw LDR vs the resulting backlight. On this board raw
  // is ~0 in room light and climbs into the hundreds/thousands only as it gets dark (§18). Handy
  // while tuning AutoBrightness::kCurve; safe to drop once the curve is dialed in.
  static uint32_t last_ldr_log = 0;
  if (static_cast<uint32_t>(now - last_ldr_log) >= 1000U) {
    last_ldr_log = now;
    Serial.printf("[ldr] raw=%4d backlight=%3u auto=%d bias=%ld awake=%d\n", g_ambient.read(),
                  g_auto_brightness.level(), static_cast<int>(g_settings.autoBrightness()),
                  static_cast<long>(bias), static_cast<int>(g_sleep.awake()));
    // Link state (§9), on the CYD's own USB console — which is free precisely because §2 put
    // the link on other pins. matched=0 with sawPeer=1 is a schema skew; sawPeer=0 means we
    // are still announcing into silence. peer= is the controller's boot nonce, so a change
    // there is a controller restart.
    Serial.printf("[link] matched=%d sawPeer=%d alive=%d state=%d nonce=%08lx peer=%08lx\n",
                  static_cast<int>(g_cyd_link.handshake().matched()),
                  static_cast<int>(g_cyd_link.handshake().sawPeer()),
                  static_cast<int>(g_cyd_link.linkAlive()),
                  static_cast<int>(lv_subject_get_int(&subj_link_state)),
                  static_cast<unsigned long>(g_cyd_link.handshake().bootNonce()),
                  static_cast<unsigned long>(g_cyd_link.handshake().peer().boot_nonce));
  }

#if defined(UI_DEV_TOOLS)
  ui_dev_tools_loop();
#endif
  delay(5);
}
