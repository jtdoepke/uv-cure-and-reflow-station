// CYD "Cheap Yellow Display" HMI firmware.
//
// LovyanGFX drives the panel + touch. Which board, which orientation, and every pin this file
// needs come from include/cyd_board.h — deliberately, so this stays the app's composition root
// and not a board definition. The app runs a startup color self-test, then shows the Home screen.
// See CLAUDE.md for build/upload commands.

#include <Arduino.h>
#include <lvgl.h>
#include "auto_brightness.h"          // ambient-light -> backlight logic (lib/app_logic, §18)
#include "cyd_board.h"                // this board's pins, capabilities, orientation, buffers
#include "cyd_link.h"                 // reliability facade (lib/protocol, §9)
#include "device_info.h"              // board/panel identity for Settings > About (lib/ui_logic)
#include "esp32_ambient_light.h"      // LDR IAmbientLight adapter (firmware glue)
#include "esp32_clock.h"              // IClock adapter for the link's cadences (firmware glue)
#include "esp32_serial_transport.h"   // ISerialTransport adapter over the link UART (firmware glue)
#include "frame_link.h"               // TinyFrame framing (lib/protocol)
#include "home_screen.h"              // UI construction lives in lib/ui_logic (host-testable)
#include "home_viewmodel.h"           // handshake -> LinkState mapper (lib/ui_logic)
#include "lgfx_display.h"             // LGFX IDisplay + IBacklight adapter (firmware glue)
#include "link_params.h"              // shared §9 cadences, incl. the FrameLink tick (lib/protocol)
#include "lgfx_touch.h"               // LGFX ITouch adapter (firmware glue)
#include "message_router.h"           // frame -> typed message dispatch (lib/protocol)
#include "null_ambient_light.h"       // IAmbientLight for a board with no LDR (firmware glue)
#include "nvs_settings_storage.h"     // NVS-backed ISettingsStorage adapter (firmware glue)
#include "littlefs_profile_storage.h" // LittleFS-backed IProfileStorage adapter (firmware glue)
#include "profile_editor_screen.h"    // §12 profile editor screen pair (lib/ui_logic, C5)
#include "profile_library_screen.h"   // §23 profile library screen pair (lib/ui_logic, C4)
#include "profile_store.h"            // per-mode profile library (lib/app_logic, §7/§23)
#include "profile_templates.h"        // per-mode default phase templates (lib/app_logic, §12/C5)
#include "screen_router.h"            // hub-and-spoke screen manager + cache policy (lib/ui_logic)
#include "settings_screen.h"          // Settings hub + panels (§24)
#include "settings_store.h"           // typed device settings (lib/app_logic)
#include "sleep_controller.h"         // idle sleep/wake policy (lib/app_logic, §17)
#include "subjects.h"
#include "schema.h" // shared wire-contract identity (lib/protocol)
#if defined(UI_DEV_TOOLS)
#include "injected_touch.h" // ITouch decorator letting the dev API pre-empt the panel
#include "ui_dev_tools.h"   // WiFi screenshot/touch API (esp32dev_cyd_uidev env only)
#endif

#if defined(PERF_PROBE)
#include <esp_timer.h>  // esp_timer_get_time() — us clock for the flush/render split
#include "perf_probe.h" // on-glass CPU/SPI split (esp32dev_cyd35_perf env only)
#endif

// Enlarge the Arduino loop task's stack. LVGL screens are built on the loop task, and a screen
// build is a deep call chain with large local buffers (the profile chart samples several
// kMaxCurvePoints/kMaxPhases arrays and loads a StoredProfile on the stack) — the 8 kB default
// overflowed opening the profile detail (Guru Meditation "Double exception", a stack-overflow
// signature). 16 kB gives comfortable headroom; it costs 8 kB of heap for the task stack (no PSRAM,
// but there is ample internal heap once the LVGL pool is carved out).
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

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

// Profile library (§7/§23), per mode, on LittleFS. Instantiated here as live production wiring so
// the store + adapter are compiled into the firmware; the library/editor screens that drive them
// are C4/C5. Separate directories keep cure and reflow profiles unmixed (§7).
static LittleFsProfileStorage g_cure_profile_storage("/profiles/cure");
static LittleFsProfileStorage g_reflow_profile_storage("/profiles/reflow");
static ProfileStore g_cure_profiles(g_cure_profile_storage, RecipeMode::Cure);
static ProfileStore g_reflow_profiles(g_reflow_profile_storage, RecipeMode::Reflow);
static ProfileLibraryScreen g_profile_library; // §23 list/detail over the two stores (C4)

// §12 editor over the two stores (C5). Heap-allocated on first edit rather than a static: the
// library's two view-models already fill the static DRAM segment (32×32 name buffers each), and the
// editor is just as large; since the two screens are never co-visible, keeping the editor out of
// .bss until it is first needed is what lets both fit on this no-PSRAM board. Never freed — a
// singleton screen (like the other resident screens), so there is no delete-during-event hazard.
static ProfileEditorScreen *g_profile_editor = nullptr;
static ProfileEditorScreen &profile_editor() {
  if (g_profile_editor == nullptr) {
    g_profile_editor = new ProfileEditorScreen();
  }
  return *g_profile_editor;
}

// The display + touch behind their ports (lib/display_port). LGFX itself is touched only here and
// in my_disp_flush — see lgfx_display.h for why the flush deliberately is not a port call.
static LgfxDisplay g_display(gfx);
static LgfxTouch g_panel_touch(gfx);
#if defined(UI_DEV_TOOLS)
static InjectedTouch g_injected_touch(g_panel_touch); // WiFi-injected touches pre-empt the panel
static ITouch &g_touch = g_injected_touch;
#else
static ITouch &g_touch = g_panel_touch;
#endif

// Auto-brightness (§18) + idle sleep/wake (§17). AutoBrightness owns the backlight; the
// SleepController decides awake/asleep and AutoBrightness ramps the backlight off/on to match.
// Fed from g_settings each loop (auto on/off, brightness bias, idle timeout).
//
// The ambient sensor is a per-board capability (cyd_board::kHasAmbientLight): a board with no LDR
// gets the null adapter and AutoBrightness held disabled, rather than an Esp32AmbientLight aimed
// at pin -1. `if constexpr` so only the real adapter is instantiated per build.
#if CYD_HAS_AMBIENT_LIGHT
static Esp32AmbientLight g_ambient(kAmbientPin);
#else
static NullAmbientLight g_ambient;
#endif
// Stock config (§18). A board with no sensor does not need a board-specific manual level here: the
// loop pushes the user's stored Screen brightness in as the manual level every iteration, before
// the first tick(), so anything set at construction would be overwritten before it ever lit a
// pixel. Its default lives with the other settings defaults, where the user can move it.
static AutoBrightness g_auto_brightness(g_ambient, g_display);
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

// Hub-and-spoke navigation via a ScreenRouter (lib/ui_logic/screen_router). Home is CACHED — built
// once and kept resident, so returning to it is an lv_screen_load, not an lv_obj_clean + rebuild.
// This is a deliberate exception to the "create-on-demand, delete on leave" rule (architecture.md):
// Home is the always-returned-to hub, it is stateless and fully subject-bound (a cached instance
// stays current — its observers fire even while off-screen — and renders identically to a fresh
// build), and rebuilding it is expensive. Measured on the 3.5" (perf/baseline/device-35.md):
// return-to-home 203 -> 114 ms — the rebuild is gone (38 -> 3 ms) AND the first render no longer
// recomputes the flex layout the clean+rebuild invalidated (163 -> 112 ms). Keeping it resident
// costs ~9-14 kB of the 64 kB LVGL pool. Settings stays create-on-demand: it is stateful (scroll /
// drill-down / selection), so a cached instance would NOT be pixel-identical to a rebuild — caching
// it would need a reset-on-show hook (ScreenRouter supports one) and is low-ROI (rarely revisited).
enum : int { SCREEN_HOME = 0, SCREEN_SETTINGS = 1, SCREEN_PROFILES = 2, SCREEN_EDITOR = 3 };
static ScreenRouter g_router;

// Which mode's library the editor returns to (the editor edits one mode's profile; on exit we land
// back on that mode's list, not the mode-blind chooser).
static RecipeMode g_editor_mode = RecipeMode::Reflow;

static ProfileStore &store_for(RecipeMode m) {
  return m == RecipeMode::Cure ? g_cure_profiles : g_reflow_profiles;
}

static void go_home() {
  lv_subject_set_int(&subj_nav_request, NAV_NONE); // reset so re-tapping a hub tile re-triggers
  g_router.show(SCREEN_HOME);
}

// Home publishes a NAV_* intent on its tiles; these observers (on the subject, so they survive
// screen swaps) route to the destination hub, whose Back fires the matching exit -> go_home.
static void on_settings_exit(void *) {
  go_home();
}

static void on_profiles_exit(void *) {
  go_home();
}

// Editor Back / completed Save → return to the edited mode's library list (not the chooser), where
// the just-saved profile is now visible (the list re-reads the store on show). Reset the nav intent
// first so re-tapping Edit on the same profile re-triggers the observer (go_home's idiom).
static void on_editor_exit(void *) {
  lv_subject_set_int(&subj_nav_request, NAV_NONE);
  g_router.show(SCREEN_PROFILES);
  g_profile_library.openMode(g_editor_mode);
}

// The profile-library NAV_PROFILE_* seam: seed the editor's working copy and route to it. NEW seeds
// the mode's default template (name entry supplies the name on Save); EDIT loads the highlighted
// profile (a stock one edits as Save-as, §23). The which-profile handoff the library reserved
// (subjects.h) is resolved here, in the composition root, off the library's own selection state.
static void open_editor_new() {
  g_editor_mode = g_profile_library.mode();
  profile_editor().beginEdit(profile_templates::defaultTemplate(g_editor_mode),
                             store_for(g_editor_mode), /*saveAs=*/true);
  g_router.show(SCREEN_EDITOR);
}

static void open_editor_edit() {
  g_editor_mode = g_profile_library.mode();
  ProfileStore::StoredProfile p;
  const int sel = g_profile_library.selected();
  if (sel < 0 || !g_profile_library.vm().loadDetail(static_cast<size_t>(sel), p)) {
    return; // nothing selected / unreadable — stay put
  }
  const bool save_as = g_profile_library.vm().editIsSaveAs(static_cast<size_t>(sel));
  profile_editor().beginEdit(p, store_for(g_editor_mode), save_as);
  g_router.show(SCREEN_EDITOR);
}

static void on_nav_request(lv_observer_t *, lv_subject_t *subject) {
  switch (lv_subject_get_int(subject)) {
  case NAV_SETTINGS:
    g_router.show(SCREEN_SETTINGS);
    break;
  case NAV_PROFILES:
    g_router.show(SCREEN_PROFILES);
    break;
  case NAV_PROFILE_NEW:
    open_editor_new();
    break;
  case NAV_PROFILE_EDIT:
    open_editor_edit();
    break;
  default:
    // NAV_CURE_SETUP/REFLOW_SETUP/CALIBRATE have no destination yet — Home publishes them and this
    // observer ignores them until Setup (§19/C6) and Calibrate land. Running a profile goes through
    // those Setup screens, not the Profiles branch (there is no Load intent).
    break;
  }
}

static void build_home_screen_cb(void *, lv_obj_t *scr) {
  create_home_screen(scr);
}

static void build_settings_screen_cb(void *, lv_obj_t *scr) {
  g_settings_screen.setExitHandler(on_settings_exit, nullptr);
  g_settings_screen.begin(scr, g_settings);
}

static void build_profile_library_cb(void *, lv_obj_t *scr) {
  g_profile_library.setExitHandler(on_profiles_exit, nullptr);
  g_profile_library.begin(scr, g_cure_profiles, g_reflow_profiles);
}

// Create-on-demand like Settings: the editor is stateful (which page / phase / field), so a cached
// instance would not be pixel-identical to a rebuild. beginEdit() sets the working copy + save
// target before the router shows this screen; render() builds the current page from that state.
static void build_profile_editor_cb(void *, lv_obj_t *scr) {
  profile_editor().setExitHandler(on_editor_exit, nullptr);
  profile_editor().render(scr);
}

// Reset-on-show for the cached profile library: the stateless two-tile chooser is its default view,
// and a cached re-show must land there (not on whatever list/detail page it was left on). The list
// re-reads the store on entry, so nothing stale is shown.
static void reset_profile_library_cb(void *) {
  g_profile_library.showChooser();
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
#if defined(PERF_PROBE)
  int64_t perf_flush_t0 = esp_timer_get_time();
#endif
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
#if defined(PERF_PROBE)
    int64_t perf_ew_t0 = esp_timer_get_time();
    gfx.endWrite(); // waits out the final DMA, closes the session
    perf_probe::note_endwrite(esp_timer_get_time() - perf_ew_t0);
#else
    gfx.endWrite(); // waits out the final DMA, closes the session
#endif
  }
  lv_display_flush_ready(disp);
#if defined(PERF_PROBE)
  perf_probe::note_flush(esp_timer_get_time() - perf_flush_t0);
#endif
#else
  // Original single-buffer synchronous path: CPU-driven SPI with a runtime RGB565
  // byte-swap (the `true`). Blocks until the whole area is pushed — which is exactly why the
  // perf_sb calibration build uses it: here flush_sum is TRUE SPI wall time, not the async
  // enqueue cost the double-buffered path measures.
  gfx.startWrite();
  gfx.setAddrWindow(area->x1, area->y1, w, h);
  gfx.pushPixels((uint16_t *)px_map, w * h, true);
  gfx.endWrite();
  lv_display_flush_ready(disp);
#if defined(PERF_PROBE)
  perf_probe::note_flush(esp_timer_get_time() - perf_flush_t0);
#endif
#endif
}

static void my_touch_read(lv_indev_t *indev, lv_indev_data_t *data) {
  // One touch read, no build-flag branch: g_touch is the panel, or the panel behind the dev-tools
  // injector, and either way it is an ITouch (see injected_touch.h).
  int x = 0, y = 0;
  if (g_touch.getTouch(&x, &y)) { // already calibrated screen coords
    // A touch always counts as activity — including a guarded one, since someone is plainly
    // there. But for the first second after a wake the screen only lights: it does not actuate
    // whatever it just drew under the finger (§17). SleepController owns that window and arms it
    // at the wake itself, so this reads as one rule rather than "was it asleep a moment ago".
    const uint32_t touch_ms = millis();
    g_sleep.noteActivity(touch_ms);
    if (g_sleep.inputGuarded(touch_ms)) {
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
  // Publish this board's hardware capabilities as data — lib/ui_logic must never see a board flag.
  lv_subject_set_int(&subj_has_ambient_light, kHasAmbientLight ? 1 : 0);
  // Same rule for the identity strings Settings > About reports: the board knows its own name, the
  // UI does not (device_info.h). Before any screen is built, so About can never render a default.
  ui_set_device_info(DeviceInfo{kBoardName, "dev", kPanelName});

  // Load persisted settings and clamp caps to the current hard-max (§4/§24), then publish the
  // cross-screen values so any consumer sees them before Settings is opened.
  g_settings.load();
  settings_publish_subjects(g_settings);

  // Mount the profile-library filesystem (§7). Format on failure so a fresh or corrupt flash still
  // boots into a usable (empty) library rather than wedging. The library/editor screens land with
  // C4/C5; the boot log exercises the whole store -> adapter -> LittleFS stack so a regression
  // surfaces here rather than the first time a screen opens.
  if (!LittleFS.begin(/*formatOnFail=*/true)) {
    Serial.println("[fs] LittleFS mount failed — profile library unavailable");
  } else {
    ProfileStore::Summary rows[ProfileStore::kMaxListed];
    Serial.printf("[profiles] cure=%u reflow=%u\n",
                  (unsigned)g_cure_profiles.list(rows, ProfileStore::kMaxListed),
                  (unsigned)g_reflow_profiles.list(rows, ProfileStore::kMaxListed));
  }

  g_router.define(SCREEN_HOME, build_home_screen_cb, nullptr, /*cached=*/true);
  g_router.define(SCREEN_SETTINGS, build_settings_screen_cb, nullptr, /*cached=*/false);
  // Cached (like Home): the chooser is stateless two tiles, and the reset hook returns a cached
  // re-show to it. The list/detail pages are rebuilt on demand and re-read the store, so caching
  // the resident screen object is safe (the §skill cacheability rule).
  g_router.define(SCREEN_PROFILES, build_profile_library_cb, nullptr, /*cached=*/true,
                  reset_profile_library_cb);
  g_router.define(SCREEN_EDITOR, build_profile_editor_cb, nullptr, /*cached=*/false);
  g_router.show(SCREEN_HOME);
  // Watch for the Home → Settings / Profiles navigation intents (persist across screen rebuilds).
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

#if defined(PERF_PROBE)
  // Drive nav from the same static functions the UI uses, so a measured round trip is the real
  // one. to_settings() publishes the intent on_nav_request observes; to_home() is go_home().
  perf_probe::begin(perf_probe::NavHooks{
      []() { lv_subject_set_int(&subj_nav_request, NAV_SETTINGS); },
      []() { go_home(); },
  });
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
  if (static_cast<uint32_t>(now - last_link_tick) >= protocol::kLinkTickMs) {
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
  // The board's capability wins over the stored preference: with no sensor there is nothing to be
  // automatic about, and the null adapter's constant 0 would otherwise read as "bright room" and
  // pin the backlight at maximum. The setting is left stored rather than clamped — it is a user
  // preference, and it is correct again the moment this firmware runs on a board that has an LDR.
  g_auto_brightness.setEnabled(kHasAmbientLight && g_settings.autoBrightness());
  // Each board gets the one brightness control that means something, and the same live-preview
  // rule: while its stepper is open, drive the backlight from the IN-PROGRESS value rather than
  // the stored one, so the screen changes as you dial it and you are never typing a number blind
  // (§18/§24). The preview ends the instant the editor commits or cancels.
  // knob_pct is whichever of the two this board actually offers, kept for the trace below so the
  // log reports the control the user has rather than a field that cannot move.
  int32_t knob_pct = 0;
  if constexpr (kHasAmbientLight) {
    // A +/- trim on the ambient reading.
    knob_pct = g_settings_screen.isEditingBrightnessBias() ? g_settings_screen.liveBrightnessBias()
                                                           : g_settings.brightnessBias();
    g_auto_brightness.setBias(knob_pct);
  } else {
    // No reading to trim, so the setting IS the level: push it as the manual nominal and leave the
    // bias at zero, or the two would stack into one brightness with two owners.
    knob_pct = g_settings_screen.isEditingScreenBrightness()
                   ? g_settings_screen.liveScreenBrightness()
                   : g_settings.screenBrightnessPct();
    g_auto_brightness.setManualPercent(knob_pct);
    g_auto_brightness.setBias(0);
  }
  g_auto_brightness.tick(now);

  // On-glass curve tuning trace (1 Hz): the raw LDR vs the resulting backlight. On a board with
  // the LDR fitted, raw is ~0 in room light and climbs into the hundreds/thousands only as it
  // gets dark (§18). Handy while tuning AutoBrightness::kCurve; safe to drop once dialed in.
  // lastRaw() rather than a fresh g_ambient.read(): it is the sample the level was actually
  // computed from, and it costs no extra ADC conversion. Reads 0 when auto is off — including on
  // a board with no sensor, where the whole line is just "what is the backlight doing".
  static uint32_t last_ldr_log = 0;
  if (static_cast<uint32_t>(now - last_ldr_log) >= 1000U) {
    last_ldr_log = now;
    // The knob is labelled for the board: "bias" is a +/- trim on the ambient reading, "bright" is
    // the absolute Screen brightness a sensorless board sets instead. Printing one under the other
    // one's name would misread a 90% brightness as a +90% trim.
    Serial.printf("[ldr] raw=%4d backlight=%3u auto=%d %s=%ld%% awake=%d\n",
                  g_auto_brightness.lastRaw(), g_auto_brightness.level(),
                  static_cast<int>(kHasAmbientLight && g_settings.autoBrightness()),
                  kHasAmbientLight ? "bias" : "bright", static_cast<long>(knob_pct),
                  static_cast<int>(g_sleep.awake()));
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
#if defined(PERF_PROBE)
  perf_probe::service(); // runs a measurement burst when a command arrives on Serial (blocking)
#endif
  delay(5);
}
