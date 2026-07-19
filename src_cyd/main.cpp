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
#include "confirm_run_screen.h"     // §19 run-confirmation screen (lib/ui_logic, C6b)
#include "cyd_link.h"               // reliability facade (lib/protocol, §9)
#include "device_info.h"            // board/panel identity for Settings > About (lib/ui_logic)
#include "esp32_ambient_light.h"    // LDR IAmbientLight adapter (firmware glue)
#include "esp32_clock.h"            // IClock adapter for the link's cadences (firmware glue)
#include "esp32_serial_transport.h" // ISerialTransport adapter over the link UART (firmware glue)
#include "fault_controller.h"       // §22 fault latch + ack routing (lib/app_logic, B7)
#include "fault_overlay.h"          // §22 fault modal on lv_layer_top (lib/ui_logic, C8)
#include "frame_link.h"             // TinyFrame framing (lib/protocol)
#include "home_screen.h"            // UI construction lives in lib/ui_logic (host-testable)
#include "home_viewmodel.h"         // handshake -> LinkState mapper (lib/ui_logic)
#include "lgfx_display.h"           // LGFX IDisplay + IBacklight adapter (firmware glue)
#include "link_params.h"            // shared §9 cadences, incl. the FrameLink tick (lib/protocol)
#include "lgfx_touch.h"             // LGFX ITouch adapter (firmware glue)
#include "message_router.h"         // frame -> typed message dispatch (lib/protocol)
#include "management_client.h"      // remote profile/settings client (lib/app_logic, §9; Wave R3b)
#include "mem_settings_storage.h"   // in-RAM ISettingsStorage (settings live on the controller now)
#include "null_ambient_light.h"     // IAmbientLight for a board with no LDR (firmware glue)
#include "profile_editor_screen.h"  // §12 profile editor screen pair (lib/ui_logic, C5)
#include "profile_library_screen.h" // §23 profile library screen pair (lib/ui_logic, C4)
#include "profile_templates.h"      // per-mode default phase templates (lib/app_logic, §12/C5)
#include "run_screen.h"             // §15 Run / Monitor screen (lib/ui_logic, C7a)
#include "screen_router.h"          // hub-and-spoke screen manager + cache policy (lib/ui_logic)
#include "settings_codec.h"         // Settings <-> oven_Settings at the wire boundary (Wave R3b)
#include "settings_screen.h"        // Settings hub + panels (§24)
#include "settings_store.h"         // typed device settings (lib/app_logic)
#include "sleep_controller.h"       // idle sleep/wake policy (lib/app_logic, §17)
#include "subjects.h"
#include "schema.h"     // shared wire-contract identity (lib/protocol)
#include "touch_safe.h" // the ONE codebase-wide touch-safe temperature (lib/calibration)
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

// Chamber temperature (°C) at/above which Home shows the HOT badge instead of IDLE while not
// running (§14/§17) — the firmware owns this policy so lib/ui_logic stays threshold-free. This is
// the touch-safe cooldown target: the single shared oven_domain::kTouchSafeC (touch_safe.h, the
// same source the controller's oven_safety::TOUCH_SAFE_C reads), not the control-side safety
// header.
static constexpr int kHomeHotThresholdC = static_cast<int>(oven_domain::kTouchSafeC);

// Device settings (§24). REVISED (Wave R3b): they persist on the CONTROLLER now (§4/§7), so the
// CYD keeps an in-RAM SettingsStore synced over the link — fetched on connect, pushed on change
// (see the settings-sync in loop()). The Settings screen still edits this local store unchanged.
static MemSettingsStorage g_settings_storage;
static SettingsStore g_settings(g_settings_storage);
static SettingsScreen g_settings_screen;

// The profile library (§23) is a remote client of the controller's store now (Wave R3b): no CYD
// ProfileStore/LittleFS. The library + editor screens drive the shared ManagementClient (defined
// below, after the link stack). Separate cure/reflow scoping lives on the controller (§7).
static ProfileLibraryScreen g_profile_library;

// The mode chosen on Home (UV Cure / Reflow, §19/C6) scopes the profile picker that opens next. The
// run flow is picker → Confirm (the one preview + HOLD-to-start page) → Run — there is no separate
// Setup screen; the library is reused in pick mode for the "Load a profile" list (one object, no
// second 32-row cache on this no-PSRAM board — see the SCREEN_PROFILES definition).
static RecipeMode g_run_mode = RecipeMode::Reflow;

// §12 editor (C5). Heap-allocated on first edit rather than a static: the library's two view-models
// already fill the static DRAM segment (32-row caches each), and the editor is just as large; since
// the two screens are never co-visible, keeping the editor out of .bss until first needed is what
// lets both fit on this no-PSRAM board. Never freed — a singleton screen, so no delete-during-event
// hazard.
static ProfileEditorScreen *g_profile_editor = nullptr;
static ProfileEditorScreen &profile_editor() {
  if (g_profile_editor == nullptr) {
    g_profile_editor = new ProfileEditorScreen();
  }
  return *g_profile_editor;
}

// §19 Confirm screen (C6b). Heap-allocated on first use like the setup/editor screens — it too owns
// a 32-phase ProfileDraft that would overflow .bss. Drives the §9 start handshake on commit.
static ConfirmRunScreen *g_confirm_screen = nullptr;
static ConfirmRunScreen &confirm_screen() {
  if (g_confirm_screen == nullptr) {
    g_confirm_screen = new ConfirmRunScreen();
  }
  return *g_confirm_screen;
}

// §15 Run / Monitor screen (C7a). Heap-allocated on first run like the other draft-owning screens —
// it holds both the authored ProfileDraft and a RunTracker (projected curve + fit), well past what
// .bss can spare on this no-PSRAM board.
static RunScreen *g_run_screen = nullptr;
static RunScreen &run_screen() {
  if (g_run_screen == nullptr) {
    g_run_screen = new RunScreen();
  }
  return *g_run_screen;
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

// §22 fault annunciation. The latch (B7) is fed from two origins in loop(): the controller's
// `Fault` frame, and our own edge-triggered link-loss during a run. The overlay lives on
// lv_layer_top, so it is deliberately NOT a router screen and needs no entry here beyond its poll.
//
// The latch is static (it is ticked every loop and must survive from boot); the OVERLAY is
// heap-allocated on first use, the same idiom as the screens above. Making it a static overflowed
// `dram0_0_seg` on the 2.8" board by 40 bytes — that panel's .bss is the binding constraint on this
// project (see the DRAM-budget note in design.md), and a view that only exists once a fault has
// been latched has no business occupying .bss for the entire uptime of a machine that never faults.
static void on_fault_ack(void *user_data, AckRoute route); // defined with the other nav handlers

// Both are heap singletons rather than statics. The LATCH is allocated at first use, which is the
// first loop iteration (it has to tick from boot to detect a link loss); the OVERLAY only when a
// fault is actually latched, so an oven that never faults never builds a view it never shows.
static FaultController *g_fault_ptr = nullptr;
static FaultController &fault_latch() {
  if (g_fault_ptr == nullptr) {
    g_fault_ptr = new FaultController();
  }
  return *g_fault_ptr;
}
static FaultOverlay *g_fault_overlay_ptr = nullptr;
static FaultOverlay &fault_overlay() {
  if (g_fault_overlay_ptr == nullptr) {
    g_fault_overlay_ptr = new FaultOverlay();
    g_fault_overlay_ptr->begin(fault_latch());
    g_fault_overlay_ptr->setAckHandler(on_fault_ack, nullptr);
  }
  return *g_fault_overlay_ptr;
}

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

// The remote profile/settings client (§9; Wave R3b). Shares the same FrameLink; CydLink forwards
// the management reply frames to it (setAppObserver in setup()). The library + editor screens and
// the settings-sync all drive it (single-outstanding, idle-context UI traffic).
static ManagementClient g_mgmt_client(g_link, g_link_clk);

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
      // A full cure profile: warm to 80 °C (convection + UV + turntable), hold, then a passive
      // cool tail to touch-safe. For the A8 dummy-load proof the controller ignores the recipe
      // content (it only needs authorize()), but for the A10 plant simulator this drives a real
      // ramp -> hold -> coast -> DONE run whose Sum(dur)x1.5 runtime budget covers the actual time.
      oven_Recipe rec = oven_Recipe_init_default;
      rec.id = 1;
      rec.mode = oven_Mode_MODE_CURE;
      rec.segments_count = 3;
      rec.segments[0].interp = oven_Interp_INTERP_RAMP_OVER_TIME;
      rec.segments[0].heat_c = 80.0F;
      rec.segments[0].dur_ms = 120000;
      rec.segments[0].conv_fan = true;
      rec.segments[0].uv = true;
      rec.segments[0].motor = true;
      rec.segments[1].interp = oven_Interp_INTERP_HOLD;
      rec.segments[1].heat_c = 80.0F;
      rec.segments[1].dur_ms = 60000;
      rec.segments[1].conv_fan = true;
      rec.segments[1].uv = true;
      rec.segments[1].motor = true;
      rec.segments[2].interp = oven_Interp_INTERP_RAMP_OVER_TIME;
      rec.segments[2].heat_c = oven_domain::kTouchSafeC; // coast to the shared touch-safe target
      // ~600 s: the real uninsulated passive coast 80 -> 43 C runs ~500 s, and the L3 runtime
      // budget is Sum(dur)x1.5, so the cool tail must be sized to the physics or the supervisor
      // trips RUNTIME_EXCEEDED during the (correct) slow cooldown. In production B1 computes this
      // from the oven_cal cool envelope; this hand-authored bench recipe matches it.
      rec.segments[2].dur_ms = 600000;
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
enum : int {
  SCREEN_HOME = 0,
  SCREEN_SETTINGS = 1,
  SCREEN_PROFILES = 2,
  SCREEN_EDITOR = 3,
  SCREEN_PICKER =
      4, // the profile library in pick mode (Home → mode → pick), same g_profile_library
  SCREEN_CONFIRM = 5, // §19 preview + HOLD-to-start (C6b)
  SCREEN_RUN = 6,     // §15 Run / Monitor (C7a)
};
static ScreenRouter g_router;

// The session of the run being started, generated when a profile is picked (on_pick_selected; §9:
// controller-unique, non-zero) and handed to Confirm, then to the Run screen on commit. The
// authored draft flows picker → Confirm → commit callback → RunScreen::begin (which copies it), no
// static.
static uint32_t g_run_session = 0;

// Which mode's library the editor returns to (the editor edits one mode's profile; on exit we land
// back on that mode's list, not the mode-blind chooser).
static RecipeMode g_editor_mode = RecipeMode::Reflow;

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

// Editor Back / completed Save → return to the edited mode's library list (reset the nav intent
// first so re-tapping the same action re-triggers the observer, go_home's idiom). The editor is
// reached only from the Profiles branch (C5) now, so it always returns there.
static void on_editor_exit(void *) {
  lv_subject_set_int(&subj_nav_request, NAV_NONE);
  g_router.show(SCREEN_PROFILES);
  g_profile_library.openMode(g_editor_mode);
}

// A profile picked (Home → mode → picker) → confirm the run over a fresh controller-unique session
// (§9). Confirm is the one preview + HOLD-to-start page; there is no intermediate Setup screen.
static void on_pick_selected(void *, const ProfileDraft &draft) {
  g_run_session = esp_random() | 1U; // non-zero: 0 means "no session"; reused by the Run screen
  lv_subject_set_int(&subj_nav_request, NAV_NONE);
  confirm_screen().begin(draft, g_run_session, g_cyd_link, g_mgmt_client);
  g_router.show(SCREEN_CONFIRM);
}

// Picker Back (nothing chosen) → Home.
static void on_picker_exit(void *) {
  go_home();
}

// Confirm Back → the picker, to choose a different profile.
static void on_confirm_exit(void *) {
  lv_subject_set_int(&subj_nav_request, NAV_NONE);
  g_router.show(SCREEN_PICKER);
}

// Confirm commit → the run is enabled on the link (§9). Hand the authored draft + session to the
// Run screen (§15), which arms a RunTracker and monitors the live telemetry.
static void on_confirm_commit(void *, const ProfileDraft &draft) {
  lv_subject_set_int(&subj_nav_request, NAV_NONE);
  run_screen().begin(draft, g_run_session, g_cyd_link);
  g_router.show(SCREEN_RUN);
}

// §15 cure Resume: Start B6's remainder as a FRESH run over a new session. It goes straight to the
// Run screen rather than back through Confirm — unlike "Run again", the deliberate-commit gesture
// has already happened here (the press-and-hold Resume on the Paused page), and §19's rule is about
// requiring that friction once, not about which screen hosts it.
static void on_run_resume(void *, const ProfileDraft &remainder) {
  const ProfileDraft draft =
      remainder; // by value: begin() re-seats the screen that owns the source
  g_run_session = esp_random() | 1U;
  lv_subject_set_int(&subj_nav_request, NAV_NONE);
  confirm_screen().begin(draft, g_run_session, g_cyd_link, g_mgmt_client);
  // show() BEFORE commit(): the router's build callback is what hands the screen its parent object,
  // and commit() immediately builds the "Starting" page. Committing first would build into a null
  // parent.
  g_router.show(SCREEN_CONFIRM);
  // The deliberate gesture already happened (the press-and-hold Resume), so drive the §9
  // Recipe+Start handshake straight away and reuse Confirm's whole commit machine — including its
  // Nak/timeout Failed page. commit() is itself gated on ready(), so if the door reopened in the
  // interim this simply leaves the operator on Confirm with a HOLD button, which is the right
  // place to be.
  confirm_screen().commit();
}

// Run summary (§16) → Home.
static void on_run_exit(void *) {
  go_home();
}

// §22 Acknowledge → where the latch says the operator belongs. RunSummary re-shows the Run screen,
// which is already sitting on its Ended page (the terminal telemetry that carried the fault put it
// there), so the aborted run keeps its §16 record instead of vanishing with the alarm.
static void on_fault_ack(void *, AckRoute route) {
  if (route == AckRoute::RunSummary) {
    lv_subject_set_int(&subj_nav_request, NAV_NONE);
    g_router.show(SCREEN_RUN);
  } else if (route == AckRoute::Home) {
    go_home();
  }
}

// Run summary → "Run again": re-confirm the SAME draft over a fresh session. Deliberately routed
// back through Confirm rather than restarting here — §19 reserves starting heat/UV for the
// press-and-hold arm, and a summary button is a plain tap.
static void on_run_again(void *) {
  const ProfileDraft draft = run_screen().draft(); // by value: Confirm's begin() outlives the copy
  g_run_session = esp_random() | 1U;
  lv_subject_set_int(&subj_nav_request, NAV_NONE);
  confirm_screen().begin(draft, g_run_session, g_cyd_link, g_mgmt_client);
  g_router.show(SCREEN_CONFIRM);
}

// The profile-library NAV_PROFILE_* seam: seed the editor's working copy and route to it. NEW seeds
// the mode's default template (name entry supplies the name on Save); EDIT loads the highlighted
// profile (a stock one edits as Save-as, §23). The which-profile handoff the library reserved
// (subjects.h) is resolved here, in the composition root, off the library's own selection state.
static void open_editor_new() {
  g_editor_mode = g_profile_library.mode();
  // NEW seeds the mode's default template locally (synchronous); name entry supplies the name on
  // Save, which pushes to the controller (§9).
  profile_editor().beginNew(profile_templates::defaultTemplate(g_editor_mode), g_mgmt_client,
                            /*saveAs=*/true);
  g_router.show(SCREEN_EDITOR);
}

static void open_editor_edit() {
  g_editor_mode = g_profile_library.mode();
  const int sel = g_profile_library.selected();
  if (sel < 0) {
    return; // nothing selected — stay put
  }
  // EDIT fetches the highlighted profile from the controller by name (the editor shows Loading); a
  // stock one edits as Save-as (§23). The name comes from the library's cached row.
  const bool save_as = g_profile_library.vm().editIsSaveAs(static_cast<size_t>(sel));
  profile_editor().beginExisting(phase_codec::modeToWire(g_editor_mode),
                                 g_profile_library.vm().name(static_cast<size_t>(sel)),
                                 g_mgmt_client, save_as);
  g_router.show(SCREEN_EDITOR);
}

// Home → UV Cure / Reflow: pick a profile for that mode straight away (§19/C6). The picker hands
// the chosen profile to Confirm (on_pick_selected); there is no separate Setup page.
static void open_run_picker(RecipeMode mode) {
  g_run_mode = mode;
  g_router.show(SCREEN_PICKER);
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
  case NAV_CURE_SETUP:
    open_run_picker(RecipeMode::Cure);
    break;
  case NAV_REFLOW_SETUP:
    open_run_picker(RecipeMode::Reflow);
    break;
  default:
    // NAV_CALIBRATE has no destination yet — Home publishes it and this observer ignores it until
    // Calibrate lands.
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
  g_profile_library.begin(scr, g_mgmt_client);
}

// The run picker (C6): the SAME g_profile_library object, entered in pick mode over g_run_mode.
// Reusing the object (not a second 32-row-cache instance) is why SCREEN_PROFILES is now create-on-
// demand — the shared object must only ever back one live widget tree at a time.
static void build_profile_picker_cb(void *, lv_obj_t *scr) {
  g_profile_library.setExitHandler(on_picker_exit, nullptr);
  g_profile_library.setPickHandler(on_pick_selected, nullptr);
  g_profile_library.beginPick(scr, g_mgmt_client, g_run_mode);
}

// Confirm (C6b): create-on-demand; open_confirm() calls begin() (draft + session + links) before
// the router shows this screen, and render() builds the current page from that state.
static void build_confirm_screen_cb(void *, lv_obj_t *scr) {
  confirm_screen().setExitHandler(on_confirm_exit, nullptr);
  confirm_screen().setCommitHandler(on_confirm_commit, nullptr);
  confirm_screen().render(scr);
}

// Run (C7a): create-on-demand; on_confirm_commit calls begin() (draft + session + link) before the
// router shows this screen, and render() builds the current page from that state.
static void build_run_screen_cb(void *, lv_obj_t *scr) {
  run_screen().setExitHandler(on_run_exit, nullptr);
  run_screen().setRunAgainHandler(on_run_again, nullptr);
  run_screen().setResumeHandler(on_run_resume, nullptr);
  run_screen().render(scr);
}

// Create-on-demand like Settings: the editor is stateful (which page / phase / field), so a cached
// instance would not be pixel-identical to a rebuild. beginEdit() sets the working copy + save
// target before the router shows this screen; render() builds the current page from that state.
static void build_profile_editor_cb(void *, lv_obj_t *scr) {
  profile_editor().setExitHandler(on_editor_exit, nullptr);
  profile_editor().render(scr);
}

// --- Settings sync (Wave R3b): settings persist on the controller; keep the local cache in step
// ---
static bool g_settings_dirty = false;   // a local edit awaits push
static bool g_settings_fetched = false; // the one-time fetch-on-connect done
enum class SettingsSync { Idle, Fetching, Pushing };
static SettingsSync g_ss_state = SettingsSync::Idle;

// A failed fetch/push must NOT re-fire every loop: that pins the one shared ManagementClient Busy
// and starves the profile screens — the very failure that made "Profiles" unreachable when
// SettingsGet went unanswered. Back off between attempts instead; the sync is background, so a few
// seconds' delay before a retry is invisible, whereas a hot spin is not.
static constexpr uint32_t kSettingsSyncRetryMs = 3000;
static uint32_t g_ss_retry_after_ms = 0; // millis() deadline gating the next sync attempt

// MemSettingsStorage fires this whenever the store writes (a settings edit) → queue a push.
static void on_settings_saved(void *) {
  g_settings_dirty = true;
}

// Copy fetched settings into the local store WITHOUT persisting (setters don't save, so no push
// loop), then republish the cross-screen subjects (units/caps/Advanced, §24).
static void adopt_settings(const oven_Settings &w) {
  const Settings s = settings_codec::fromWire(w);
  g_settings.setUnits(s.units);
  g_settings.setAutoBrightness(s.autoBrightness);
  g_settings.setAdvancedUnlocked(s.advancedUnlocked);
  g_settings.setBrightnessBias(s.brightnessBias);
  g_settings.setScreenBrightnessPct(s.screenBrightnessPct);
  g_settings.setIdleTimeoutMin(s.idleTimeoutMin);
  g_settings.setUvMaxCap(s.uvMaxCap);
  g_settings.setReflowMaxCap(s.reflowMaxCap);
  settings_publish_subjects(g_settings);
}

// Drive fetch-on-connect + push-on-change over the shared client, only when it is free — the
// profile screens have priority; settings sync is background (fetch is one-time at boot; a push
// happens while on the Settings screen, clear of the profile screens). Call every loop, BEFORE
// poll_screens() so a screen's own reply is left for the screen to consume.
static void service_settings_sync() {
  using Op = ManagementClient::Op;
  if (g_ss_state == SettingsSync::Fetching) {
    if (g_mgmt_client.busy()) {
      return;
    }
    if (g_mgmt_client.ready() && g_mgmt_client.lastOp() == Op::SettingsGet) {
      adopt_settings(g_mgmt_client.settings());
      g_settings_fetched = true;
    } else {
      g_ss_retry_after_ms = millis() + kSettingsSyncRetryMs; // failed: back off, don't spin
    }
    g_mgmt_client.clear();
    g_ss_state = SettingsSync::Idle;
    return;
  }
  if (g_ss_state == SettingsSync::Pushing) {
    if (g_mgmt_client.busy()) {
      return;
    }
    if (!g_mgmt_client.ready()) {
      g_settings_dirty =
          true; // push failed: keep the edit and retry after a backoff, don't lose it
      g_ss_retry_after_ms = millis() + kSettingsSyncRetryMs;
    }
    g_mgmt_client.clear();
    g_ss_state = SettingsSync::Idle;
    return;
  }
  if (!g_mgmt_client.idle() || !g_cyd_link.linkAlive()) {
    return; // client busy with a screen's request, or no controller yet
  }
  if (static_cast<int32_t>(millis() - g_ss_retry_after_ms) < 0) {
    return; // within the post-failure backoff window (wrap-safe deadline compare)
  }
  if (!g_settings_fetched) {
    if (g_mgmt_client.requestSettingsGet()) {
      g_ss_state = SettingsSync::Fetching;
    }
  } else if (g_settings_dirty) {
    g_settings_dirty = false;
    if (g_mgmt_client.requestSettingsPut(settings_codec::toWire(g_settings.values()))) {
      g_ss_state = SettingsSync::Pushing;
    } else {
      g_settings_dirty = true; // client busy — retry next loop
    }
  }
}

// Poll the async profile screens (Wave R3b): consume a completed list/detail/action or editor
// fetch/save and rebuild. Each is a no-op unless that screen is awaiting a reply.
static void poll_screens() {
  g_profile_library.poll();
  if (g_profile_editor != nullptr) {
    g_profile_editor->poll();
  }
  // Confirm's poll drives the live TC gate + the start handshake against build-local widgets, so it
  // must run ONLY while Confirm is the active screen (its widget pointers dangle once navigated
  // away). The commit completes and hands off before that, so nothing is lost by gating it here.
  if (g_confirm_screen != nullptr && g_router.current() == SCREEN_CONFIRM) {
    g_confirm_screen->poll();
  }
  // The Run screen's poll feeds telemetry to its RunTracker + refreshes build-local widgets, so it
  // too runs only while active (its widget pointers dangle once navigated away).
  if (g_run_screen != nullptr && g_router.current() == SCREEN_RUN) {
    g_run_screen->poll();
  }
}

// Wake the display the instant the machine leaves idle — a HOT chamber, a run start, or a
// fault must never be hidden behind a dark screen (§17/§22). The loop's sleep tick also keeps
// us awake while non-idle; this observer just makes the wake immediate on the state change.
static void on_run_state(lv_observer_t *, lv_subject_t *subject) {
  if (lv_subject_get_int(subject) != RUN_IDLE) {
    g_sleep.noteActivity(millis());
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
  // Poll touch faster than the 33 ms default (LV_DEF_REFR_PERIOD). At 33 ms two quick taps that
  // land in one window coalesce, which reads as keyboard lag when typing at speed; ~16 ms doubles
  // the sample rate so fast key taps register as distinct presses. The read itself is cheap (an
  // XPT2046 SPI sample), so the extra polls cost little.
  lv_timer_set_period(lv_indev_get_read_timer(indev), 16);

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

  // Start on the factory-default settings, publishing the cross-screen values so any consumer sees
  // them before Settings opens. The real values are FETCHED from the controller once the link is up
  // (settings live there now, §4/§7; service_settings_sync in loop), then re-published.
  g_settings.load(); // in-RAM cache empty → defaults
  settings_publish_subjects(g_settings);
  g_settings_storage.setOnSaved(on_settings_saved, nullptr); // a settings edit → queue a push

  g_router.define(SCREEN_HOME, build_home_screen_cb, nullptr, /*cached=*/true);
  g_router.define(SCREEN_SETTINGS, build_settings_screen_cb, nullptr, /*cached=*/false);
  // Create-on-demand: the library object is now SHARED with the Setup picker (SCREEN_PICKER), so it
  // must only ever back one live widget tree — a cached SCREEN_PROFILES tree would go stale the
  // moment the picker re-points the object's parent_ (C6). begin() rebuilds fresh (chooser + a
  // store re-read) on each show, so nothing stale is shown; the cost is one rebuild of a
  // rarely-revisited screen (Home, the always-returned hub, stays cached).
  g_router.define(SCREEN_PROFILES, build_profile_library_cb, nullptr, /*cached=*/false);
  g_router.define(SCREEN_EDITOR, build_profile_editor_cb, nullptr, /*cached=*/false);
  g_router.define(SCREEN_PICKER, build_profile_picker_cb, nullptr, /*cached=*/false);
  g_router.define(SCREEN_CONFIRM, build_confirm_screen_cb, nullptr, /*cached=*/false);
  g_router.define(SCREEN_RUN, build_run_screen_cb, nullptr, /*cached=*/false);
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
  // The management client shares the link: CydLink forwards profile/settings reply frames to it,
  // and it seeds its own seq base per boot (same dedup reason as the setup path, §9; Wave R3b).
  g_mgmt_client.setSeqBase(esp_random());
  g_cyd_link.setAppObserver(g_mgmt_client);
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
  // Profile/settings remote client (§9; Wave R3b): retry/timeout, then the background settings sync
  // (before poll_screens so a screen's own reply is left for the screen), then the async screen
  // polls (list/detail/action/editor fetch/save land here).
  g_mgmt_client.service();
  service_settings_sync();
  poll_screens();
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

  // Drive Home's live chamber temp + run-state badge from the controller's telemetry (§14) — the
  // "controller-link integration" the earlier placeholders anticipated. Only while the link is
  // alive AND a frame has arrived: a stale frame past the timeout is no longer the machine's real
  // state (the link banner already flags that). The chamber reading is the hottest wall channel —
  // the conservative "how hot is it in there" number, matching the L3 high-limit; the run-state
  // badge folds the wire run_state + fault code + a touch-safe HOT check into Home's RunState.
  if (g_cyd_link.linkAlive() && g_cyd_link.hasTelemetry()) {
    const oven_Telemetry &t = g_cyd_link.lastTelemetry();
    if (t.wall_temp_count > 0) {
      float hottest = t.wall_temp[0];
      for (pb_size_t i = 1; i < t.wall_temp_count; ++i) {
        if (t.wall_temp[i] > hottest) {
          hottest = t.wall_temp[i];
        }
      }
      lv_subject_set_int(&subj_chamber_temp, static_cast<int>(lroundf(hottest)));
    }
    // Raw wire read (protocol::wireEnum): an enum-typed load of a value the peer left outside the
    // enumerators is UB, and a skewed schema is exactly how that happens (fault_table.h).
    const fault_table::FaultCodeWire fault_wire = protocol::wireEnum(t.fault_code);
    const bool faulted =
        fault_wire != oven_FaultCode_FAULT_NONE || t.run_state == oven_RunState_RUN_STATE_FAULT;
    const bool running = t.run_state == oven_RunState_RUN_STATE_RUNNING;
    const bool hot = lv_subject_get_int(&subj_chamber_temp) > kHomeHotThresholdC;
    lv_subject_set_int(&subj_run_state, HomeViewModel::runStateFrom(running, faulted, hot));
    // §17 door-open wake — the TODO the controller-link integration was waiting on. Opening the
    // door is someone walking up to the machine, so the screen should already be lit when they
    // look at it; the controller sends telemetry immediately on the door edge (§9) rather than
    // waiting out its 250 ms cadence, which is what makes this feel instant.
    //
    // An edge check here rather than a shared lv_subject_t: nothing in lib/ui_logic binds to the
    // door (the Confirm gate and the Run screen both read the telemetry frame directly), so a
    // subject would be a publish with exactly one subscriber three lines below — and on the 2.8"
    // board it cost 8 bytes more .bss than that segment had left.
    static bool door_prev = false;
    if (t.door_open && !door_prev) {
      g_sleep.noteActivity(now);
    }
    door_prev = t.door_open;

    // §22 origin 1 — the controller safed itself and said why. Read from telemetry's `fault_code`
    // rather than the dedicated `Fault` frame because CydLink forwards content to ONE app observer
    // and the ManagementClient holds it (request/reply correlation). A4b built `fault_code` to ride
    // continuously for exactly this reason — a backup channel that cannot be missed — and the
    // ≤250 ms it costs is nothing against a human reading a modal. If the dedicated frame is ever
    // needed here, chaining the observer is the change.
    //
    // EDGE-triggered: FaultController counts every call toward its `+N`, and this runs every loop,
    // so a level-triggered feed would show "+40000 more" within a second of a single fault.
    fault_latch().setRunActive(
        running); // before the raise — the latch captures it to pick the ack route
    static fault_table::FaultCodeWire last_fault_code = oven_FaultCode_FAULT_NONE;
    if (fault_wire != last_fault_code) {
      last_fault_code = fault_wire;
      if (fault_wire != oven_FaultCode_FAULT_NONE) {
        fault_latch().onControllerFault(now, t.session, fault_wire);
        g_sleep.noteActivity(now); // §22: never hide a fault behind a dark backlight
      }
    }
  }

  // §22 origin 2 — our own heartbeat went silent mid-run, so no `Fault` can reach us. Ticked
  // unconditionally (outside the telemetry block above): a dead link is precisely the case where
  // that block stops running, so gating this on it would disable the one detector that covers it.
  fault_latch().tick(now, g_cyd_link.linkAlive());
  // Only touch the overlay once there is something to show (or once it has been built), so an oven
  // that never faults never allocates it. After the first fault it stays around — rebuilding a view
  // on every alarm would be the wrong trade.
  if (fault_latch().active() || g_fault_overlay_ptr != nullptr) {
    const bool fault_was_visible = fault_overlay().visible();
    fault_overlay().poll();
    if (fault_overlay().visible() && !fault_was_visible) {
      g_sleep.noteActivity(now); // a self-raised LINK_LOST wakes the screen too
    }
  }

  lv_tick_inc(now - last_tick);
  last_tick = now;
  lv_timer_handler();

  // Sleep/wake (§17) + auto-brightness (§18). The screen may sleep ONLY when the machine is at
  // rest — HomeViewModel::atRest is the same predicate the green idle dot uses (idle AND touch-safe
  // AND unfaulted), so the two can never drift. Never sleep during a run (a dark screen would stall
  // the heartbeat -> controller aborts to safe, §9) or while the chamber is still hot (§17/§22).
  // Wake-on-hot-while-asleep is covered twice over: the on_run_state observer fires noteActivity()
  // the instant run_state leaves IDLE (an IDLE->HOT transition when the chamber crosses
  // touch-safe), and this tick() also force-wakes whenever sleep_allowed is false — so a sleeping
  // screen relights as soon as the telemetry shows the chamber above touch-safe.
  // Also never sleep while the Run/Monitor screen is up — including its terminal "Run complete" /
  // "Fault - run ended" page (§15/§16): the operator must see the outcome until they dismiss it,
  // and a completed run whose chamber has cooled to touch-safe would otherwise read as at-rest and
  // time out to a dark screen. Leaving SCREEN_RUN (Done -> Home) restores the normal idle timeout.
  // Also never while a fault is unacknowledged, and not while an over-temp stays latched past the
  // acknowledge (§22: "the HOT state persists on Home and keeps suppressing sleep until the chamber
  // cools"). The over-temp latch is cleared once the chamber reads touch-safe again — the same
  // threshold the idle dot uses, so the two cannot drift.
  if (fault_latch().overTempLatched() &&
      lv_subject_get_int(&subj_chamber_temp) < static_cast<int>(oven_domain::kTouchSafeC)) {
    fault_latch().clearOverTemp();
  }
  bool sleep_allowed = HomeViewModel::atRest(lv_subject_get_int(&subj_run_state)) &&
                       g_router.current() != SCREEN_RUN && !fault_latch().active() &&
                       !fault_latch().overTempLatched();
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
    // What Home is actually showing, driven by the controller's telemetry (§14): run-state badge
    // (0=IDLE 1=HOT 2=RUNNING 3=FAULT) and the live chamber temp.
    Serial.printf("[home] run_state=%d chamber=%dC\n",
                  static_cast<int>(lv_subject_get_int(&subj_run_state)),
                  static_cast<int>(lv_subject_get_int(&subj_chamber_temp)));
  }

#if defined(UI_DEV_TOOLS)
  ui_dev_tools_loop();
#endif
#if defined(PERF_PROBE)
  perf_probe::service(); // runs a measurement burst when a command arrives on Serial (blocking)
#endif
  delay(5);
}
