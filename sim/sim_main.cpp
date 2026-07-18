// Headless UI simulator (env native_sim) — the Claude-assisted UI development loop.
//
// Renders the real lib/ui_logic widgets on LVGL's in-memory test display (same RGB565
// rasterizer as the firmware), optionally injects scripted pointer events, and writes
// PNG screenshots. No display server, no hardware. See the ui-development skill.
//
// Usage: program [--out PATH] [--screen
// home|stepper|keypad|list|settings|alerts|curve|profile-library|editor] [ACTION...]
//   click X Y | press X Y | moveto X Y | release | wait MS | shot PATH | frame PATH
//   temp N | state idle|hot|running|fault | link ok|none|schema | sensor on|off
// The temp/state/link actions drive the shared UI subjects so a screenshot can capture any
// machine/link state (the real firmware fills these from telemetry; here we set them by hand).
// --screen picks which lib/ui_logic screen to render (default: home); `stepper` shows a demo
// value-stepper editor (§24, C2), `keypad` a demo numeric keypad (§26, C1), and `list` a demo
// selectable list (§23/§24) so their layout can be reviewed without a hosting Settings panel.
// A final screenshot is always written to --out (default .pio/sim/ui.png).
// Exit codes: 0 ok, 1 usage error, 2 PNG write failure.

#include <lvgl.h>
#include "src/debugging/test/lv_test.h" // lv_test_* helpers (gated by LV_USE_TEST)

// LVGL's vendored lodepng.h wraps its C++ overload section in extern "C", which breaks
// under C++; we only need the C API, so suppress the C++ section for this TU.
#define LODEPNG_NO_COMPILE_CPP
#include "src/libs/lodepng/lodepng.h" // vendored in the lvgl package (LV_USE_LODEPNG)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "device_info.h"
#include "device_settings.h"    // control::SettingsStore (the controller side of the fake stack)
#include "frame_link.h"         // TinyFrame link (lib/protocol)
#include "helpers/fake_clock.h" // IClock for the ManagementClient (test helper; -I test)
#include "helpers/pipe_transport.h" // in-process LoopbackPipe joining the two link ends
#include "home_screen.h"
#include "management_client.h"    // CYD-side remote client the screens drive (§9; Wave R3b)
#include "management_responder.h" // controller-side responder answering over the pipe
#include "message_router.h"       // frame → typed dispatch (lib/protocol)
#include "numeric_keypad.h"
#include "oven_cal.h"
#include "panel.h"
#include "phase.h"
#include "phase_codec.h" // Phase[] → oven_Profile for seeding the controller store
#include "profile_curve.h"
#include "profile_editor_screen.h"
#include "profile_facts.h"
#include "profile_library.h" // control::ProfileStore (lib/control_logic — the store lives here now)
#include "profile_library_screen.h"
#include "profile_templates.h"
#include "selectable_list.h"
#include "settings_screen.h"
#include "subjects.h"
#include "theme.h"
#include "value_stepper.h"

// --- `--screen alerts`: a STYLE SPECIMEN, not a product screen -------------------------------
//
// Shows the whole caution/alarm/fault vocabulary on one canvas so the reserved hues can be judged
// against each other and against the accent — the thing you cannot see on Home, where only one
// state is ever live at a time. The real fault overlay (design.md §22) has a taxonomy, an
// acknowledge path and subjects behind it; this is its palette, not its behaviour.
//
// Lives here rather than in lib/ui_logic for exactly that reason: the STYLING it demonstrates is
// in theme.cpp (apply_alert / apply_pill / apply_fault_panel), so §22 will inherit it. Only the
// arrangement is sim-only.
static void build_alert_specimen(lv_obj_t *scr) {
  theme::apply_screen(scr); // includes the dot-matrix background
  lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(scr, theme::PAD_S, 0);
  lv_obj_set_style_pad_row(scr, theme::GAP, 0);
  lv_obj_add_flag(scr, LV_OBJ_FLAG_SCROLLABLE); // the 2.8" panel cannot hold the whole vocabulary

  auto caption = [&](const char *text) {
    lv_obj_t *l = lv_label_create(scr);
    lv_label_set_text(l, text);
    theme::apply_caption(l);
  };

  // Status pills — the at-a-glance machine state (§14). Each is glyph + word + colour, so it
  // survives being read by the ~1-in-12 men with red-green colour deficiency, or in greyscale.
  caption("STATUS PILLS");
  lv_obj_t *pills = lv_obj_create(scr);
  theme::apply_row(pills);
  lv_obj_set_width(pills, lv_pct(100));
  lv_obj_set_height(pills, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(pills, LV_FLEX_FLOW_ROW);

  struct PillSpec {
    const char *text;
    uint32_t hue;
  };
  const PillSpec pill_specs[] = {
      {LV_SYMBOL_OK " READY", theme::IDLE},
      {LV_SYMBOL_WARNING " HOT", theme::WARN},
      {LV_SYMBOL_CLOSE " FAULT", theme::FAULT},
  };
  for (const PillSpec &spec : pill_specs) {
    lv_obj_t *p = lv_obj_create(pills);
    theme::apply_pill(p, spec.hue);
    lv_obj_set_size(p, LV_SIZE_CONTENT, theme::SECONDARY_H / 2);
    lv_obj_t *t = lv_label_create(p);
    lv_label_set_text(t, spec.text);
    lv_obj_center(t);
  }

  // Caution (amber) — abnormal but not dangerous; the run continues.
  caption("CAUTION - AMBER, STEADY");
  lv_obj_t *caution = lv_obj_create(scr);
  theme::apply_alert(caution, theme::WARN);
  lv_obj_set_width(caution, lv_pct(100));
  lv_obj_set_height(caution, LV_SIZE_CONTENT);
  lv_obj_t *ct = lv_label_create(caution);
  lv_label_set_text(ct, LV_SYMBOL_WARNING "  CAUTION - Door open");
  lv_obj_center(ct);

  // Alarm (red) — dangerous, live, and the one thing on the panel allowed to move.
  caption("ALARM - RED, PULSING FILL");
  lv_obj_t *alarm = lv_obj_create(scr);
  theme::apply_alert(alarm, theme::FAULT);
  lv_obj_set_width(alarm, lv_pct(100));
  lv_obj_set_height(alarm, LV_SIZE_CONTENT);
  lv_obj_t *at = lv_label_create(alarm);
  lv_label_set_text(at, LV_SYMBOL_WARNING "  ALARM - Over-temperature");
  lv_obj_center(at);
  theme::alarm_pulse(alarm);

  // The modal danger panel (§22): big plain-language cause first, code second, ack target last.
  caption("FAULT OVERLAY - THE MODAL");
  lv_obj_t *fault = lv_obj_create(scr);
  theme::apply_fault_panel(fault);
  lv_obj_set_width(fault, lv_pct(100));
  lv_obj_set_height(fault, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(fault, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(fault, theme::PAD_S, 0);

  lv_obj_t *ftag = lv_label_create(fault);
  // ⚠ stays at the DEFAULT font size: big_font() carries digits/✓/✗/⌫ only, so a warning glyph
  // set in it would render as a missing-glyph box.
  lv_label_set_text(ftag, LV_SYMBOL_WARNING " FAULT");
  lv_obj_set_style_text_color(ftag, theme::col(theme::FAULT), 0);

  lv_obj_t *fcause = lv_label_create(fault);
  lv_label_set_text(fcause, "Chamber over-temp");
  lv_obj_set_width(fcause, lv_pct(100));
  lv_label_set_long_mode(fcause, LV_LABEL_LONG_WRAP); // §22's causes are a table, not authored here
  lv_obj_set_style_text_font(fcause, &theme::big_font(), 0);
  lv_obj_set_style_text_color(fcause, theme::col(theme::TEXT), 0);

  lv_obj_t *fcode = lv_label_create(fault);
  lv_label_set_text(fcode, "Heater and UV are off.\nCode 3 - OVER_TEMP");
  theme::apply_caption(fcode);

  lv_obj_t *ack = lv_button_create(fault);
  theme::apply_secondary(ack);
  lv_obj_set_width(ack, lv_pct(100));
  lv_obj_set_height(ack, theme::TOUCH_MIN); // the design guide's 10 mm floor, not a token guess
  lv_obj_t *acklbl = lv_label_create(ack);
  lv_label_set_text(acklbl, "Acknowledge");
  lv_obj_center(acklbl);
}

// --- `--screen curve`: the read-only profile curve widget (§12/§23, C4) ----------------------
//
// A representative multi-phase reflow profile rendered through profile_curve + profile_facts, so
// the requested/achievable line style, the instrument frame, and the "uncalibrated" note can be
// eyeballed before the full library detail screen hosts it. Constants are illustrative, not a real
// paste; oven_cal::kDefaultModel is uncalibrated, so the achievable curve rate-limits away from the
// requested one (the divergence the §12 preview exists to show).
static void build_curve_demo(lv_obj_t *scr) {
  theme::apply_screen(scr);
  lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(scr, theme::PAD_S, 0);
  lv_obj_set_style_pad_row(scr, theme::GAP, 0);

  Phase reflow[4] = {};
  reflow[0].targetC = 150.0f;
  reflow[0].rampSeconds = 90.0f;
  reflow[0].holdSeconds = 90.0f; // preheat + soak
  reflow[1].targetC = 180.0f;
  reflow[1].rampSeconds = 60.0f;
  reflow[1].holdSeconds = 60.0f;
  reflow[2].targetC = 245.0f;
  reflow[2].rampSeconds = 35.0f; // deliberately faster than the stub envelope → rate-limited
  reflow[2].holdSeconds = 30.0f; // reflow peak
  reflow[3].targetC = 50.0f;
  reflow[3].rampSeconds = 0.0f; // ASAP cool-down

  static profile_facts::CurvePoint req[profile_facts::kMaxCurvePoints];
  static profile_facts::CurvePoint over[profile_facts::kMaxCurvePoints];
  const size_t nr =
      profile_facts::sampleCurve(reflow, 4, RecipeMode::Reflow, oven_cal::kDefaultModel,
                                 /*achievable=*/false, 25.0f, req, profile_facts::kMaxCurvePoints);
  const size_t no =
      profile_facts::sampleOvershoot(reflow, 4, RecipeMode::Reflow, oven_cal::kDefaultModel, 25.0f,
                                     over, profile_facts::kMaxCurvePoints);

  const profile_facts::ProfileFacts f =
      profile_facts::computeFacts(reflow, 4, RecipeMode::Reflow, oven_cal::kDefaultModel);
  char peak[24];
  char dur[24];
  profile_facts::formatPeak(f.peakC, /*fahrenheit=*/false, peak, sizeof(peak));
  profile_facts::formatDuration(f.totalSeconds, dur, sizeof(dur));

  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, "LF-245");
  ProfileCurveData cd;
  cd.requested = req;
  cd.n_requested = nr;
  cd.overshoot = over;
  cd.n_overshoot = no;
  cd.uncalibrated = !oven_cal::CALIBRATED;
  lv_obj_t *curve = create_profile_curve(scr, cd).root;
  (void)curve;

  char facts[48];
  std::snprintf(facts, sizeof(facts), "%s \xC2\xB7 %s \xC2\xB7 4 phases", peak,
                dur); // · separators
  lv_obj_t *facts_label = lv_label_create(scr);
  lv_label_set_text(facts_label, facts);
  theme::apply_caption(facts_label);
}

// Non-persistent in-memory storage so the sim can build a SettingsStore (layout review only —
// nothing is saved across runs).
struct SimSettingsStorage : ISettingsStorage {
  size_t load(uint8_t *, size_t) override { return 0; }
  bool save(const uint8_t *, size_t) override { return true; }
};

// In-memory IProfileStorage for the profile-library demo — the sim seeds a handful of profiles at
// build time so the list/detail can be reviewed without LittleFS (its own storage, not a test
// helper, matching SimSettingsStorage above).
struct SimProfileStorage : IProfileStorage {
  std::map<std::string, std::vector<uint8_t>> blobs;
  size_t list(ProfileEntry *out, size_t cap) override {
    size_t i = 0;
    for (auto &kv : blobs) {
      if (i < cap) {
        std::strncpy(out[i].name, kv.first.c_str(), kProfileNameCap - 1);
        out[i].name[kProfileNameCap - 1] = '\0';
      }
      ++i;
    }
    return blobs.size();
  }
  size_t read(const char *name, uint8_t *buf, size_t cap) override {
    auto it = blobs.find(name);
    if (it == blobs.end() || it->second.size() > cap) {
      return 0;
    }
    std::memcpy(buf, it->second.data(), it->second.size());
    return it->second.size();
  }
  bool write(const char *name, const uint8_t *buf, size_t len) override {
    blobs[name].assign(buf, buf + len);
    return true;
  }
  bool remove(const char *name) override { return blobs.erase(name) > 0; }
};

// Seed one profile through the real control::ProfileStore::save() (PRO2 nanopb layout, exactly
// production's) so the sim's library is served over the link like the hardware's. Phase[] → oven
// via the same phase_codec the CYD editor uses.
static void seed_profile(control::ProfileStore &store, RecipeMode mode, const char *name,
                         bool stock, const Phase *phases, size_t count) {
  store.save(phase_codec::profileToWire(name, mode, stock, phases, count));
}

// The env sets LODEPNG_NO_COMPILE_ALLOCATORS: LVGL's copy would otherwise route these
// through lv_malloc, whose small builtin pool can't hold zlib's compression state.
extern "C" void *lodepng_malloc(size_t size) {
  return std::malloc(size);
}
extern "C" void *lodepng_realloc(void *ptr, size_t new_size) {
  return std::realloc(ptr, new_size);
}
extern "C" void lodepng_free(void *ptr) {
  std::free(ptr);
}

static lv_display_t *sim_disp = nullptr;

// The profile-library/editor screens are async remote clients: a fetch (openMode/openDetail/
// beginExisting) sends a frame and the reply lands a poll later. These are set when such a screen
// is active so settle() can pump the in-process link and the screen's poll() until the request
// resolves and the screen rebuilds — the sim's stand-in for the firmware loop's per-iteration
// service. Null for every other screen (nothing to pump).
static protocol::FrameLink *g_link_cyd = nullptr;
static protocol::FrameLink *g_link_ctrl = nullptr;
static ManagementClient *g_client = nullptr;
static ProfileLibraryScreen *g_profiles_screen = nullptr;
static ProfileEditorScreen *g_editor_screen = nullptr;

// Pump both link directions + the active screen's poll() until it settles (bounded). Synchronous
// pipe, so a handful of iterations resolves any single outstanding request.
static void pump_link() {
  if (g_link_cyd == nullptr) {
    return;
  }
  for (int k = 0; k < 12; ++k) {
    g_link_ctrl->poll();
    g_link_cyd->poll();
    if (g_client != nullptr) {
      g_client->service();
    }
    if (g_profiles_screen != nullptr) {
      g_profiles_screen->poll();
    }
    if (g_editor_screen != nullptr) {
      g_editor_screen->poll();
    }
  }
}

// Run LVGL until nothing is animating, so a screenshot shows a settled UI.
//
// This is not a nicety. LVGL's default theme ANIMATES style changes over
// LV_THEME_DEFAULT_TRANSITION_TIME (80 ms, lv_conf.h) — and our mode tiles change state the
// moment the link subject moves, because subjects boot at LINK_NONE and the tiles bind
// LV_STATE_DISABLED to it. Screenshot immediately after `link ok` and you catch the tile
// mid-blend: it renders a pale #7b7d84 rather than the theme's dark TILE, which reads exactly like
// a washed-out palette bug that isn't there. (Worse, the theme's disabled treatment is a 50%
// `recolor` — a post-process, so it does not even resemble the bg_color the theme sets, and
// grepping theme.h for the colour you can see finds nothing.)
//
// A screenshot tool that renders a transient state is a tool that lies, and `make sim-shot`
// exists to be believed. Settle rather than make every caller remember `wait`.
static void settle(uint32_t max_ms = 1000) {
  pump_link(); // resolve any pending profile-screen fetch first (it may rebuild the screen)
  uint32_t waited = 0;
  while (lv_anim_count_running() > 0 && waited < max_ms) {
    lv_test_wait(20);
    waited += 20;
  }
  lv_test_wait(20); // one more frame so the settled values are actually rendered
}

// Render pending changes, then encode the display's full-frame buffer as a 24-bit PNG.
// The test display runs in DIRECT render mode; we set its color format to RGB565 so the
// rasterizer output matches the firmware exactly, and expand to RGB888 only here.
// `do_settle=false` is the `frame` action: capture RIGHT NOW, mid-animation. It exists only to
// photograph motion — an infinite animation (the §22 alarm pulse) never settles, so `shot` waits
// out its 1 s bound and captures whatever phase that lands on. Sampling a sequence of `frame`s
// across a known period is the only honest way to show a pulse in a still medium. Everything
// else must keep using `shot`: settling is what stops a screenshot reporting a transient blend
// as a resting colour.
static bool write_png(const char *path, bool do_settle = true) {
  if (do_settle) {
    settle();
  }
  lv_refr_now(nullptr);
  lv_draw_buf_t *frame = lv_display_get_buf_active(sim_disp);
  if (frame == nullptr || frame->header.cf != LV_COLOR_FORMAT_RGB565) {
    std::fprintf(stderr, "ERROR: unexpected display buffer state\n");
    return false;
  }

  const uint32_t w = frame->header.w;
  const uint32_t h = frame->header.h;
  const uint32_t stride = frame->header.stride; // bytes per row, may exceed w*2
  std::vector<unsigned char> rgb(static_cast<size_t>(w) * h * 3);

  for (uint32_t y = 0; y < h; y++) {
    const uint8_t *row = frame->data + static_cast<size_t>(y) * stride;
    for (uint32_t x = 0; x < w; x++) {
      uint16_t px;
      std::memcpy(&px, row + static_cast<size_t>(x) * 2, 2);
      unsigned char *out = &rgb[(static_cast<size_t>(y) * w + x) * 3];
      out[0] = static_cast<unsigned char>(((px >> 11) & 0x1F) << 3 | ((px >> 13) & 0x07));
      out[1] = static_cast<unsigned char>(((px >> 5) & 0x3F) << 2 | ((px >> 9) & 0x03));
      out[2] = static_cast<unsigned char>((px & 0x1F) << 3 | ((px >> 2) & 0x07));
    }
  }

  // Encode in memory and write with stdio — LVGL's lodepng routes its *_file variants
  // through lv_fs driver letters, which aren't set up (or wanted) here.
  unsigned char *png = nullptr;
  size_t png_size = 0;
  unsigned err = lodepng_encode24(&png, &png_size, rgb.data(), w, h);
  if (err != 0) {
    std::fprintf(stderr, "ERROR: PNG encode failed for %s: %s\n", path, lodepng_error_text(err));
    return false;
  }
  std::FILE *f = std::fopen(path, "wb");
  bool ok = f != nullptr && std::fwrite(png, 1, png_size, f) == png_size;
  if (f != nullptr)
    std::fclose(f);
  lodepng_free(png);
  if (!ok) {
    std::fprintf(stderr, "ERROR: cannot write %s\n", path);
    return false;
  }
  std::printf("WROTE %s\n", path);
  return true;
}

static bool parse_i32(const char *s, int32_t *out) {
  char *end = nullptr;
  long v = std::strtol(s, &end, 10);
  if (end == s || *end != '\0')
    return false;
  *out = static_cast<int32_t>(v);
  return true;
}

static int usage(const char *argv0) {
  std::fprintf(stderr,
               "Usage: %s [--out PATH] [--screen "
               "home|stepper|keypad|list|settings|alerts|curve|profile-library|editor]\n"
               "          [ACTION...]\n"
               "Actions: click X Y | press X Y | moveto X Y | release | wait MS | shot PATH\n"
               "         frame PATH (unsettled capture - for photographing animation)\n"
               "         temp N | state idle|hot|running|fault | link ok|none|schema\n"
               "         sensor on|off (ambient-light sensor fitted; off = the 3.5\" board)\n",
               argv0);
  return 1;
}

int main(int argc, char **argv) {
  const char *out_path = ".pio/sim/ui.png";
  std::string screen = "home";
  std::vector<std::string> tokens;
  for (int i = 1; i < argc; i++) {
    if (std::strcmp(argv[i], "--out") == 0) {
      if (i + 1 >= argc)
        return usage(argv[0]);
      out_path = argv[++i];
    } else if (std::strcmp(argv[i], "--screen") == 0) {
      if (i + 1 >= argc)
        return usage(argv[0]);
      screen = argv[++i];
    } else {
      tokens.emplace_back(argv[i]);
    }
  }

  lv_init();
  sim_disp = lv_test_display_create(panel::W, panel::H);
  // The test display defaults to XRGB8888; render RGB565 like the firmware instead
  // (the display reallocates its full-frame buffer on this event).
  lv_display_set_color_format(sim_disp, LV_COLOR_FORMAT_RGB565);
  lv_test_indev_create_all();
  ui_subjects_init();
  // About reports what the FIRMWARE injects (device_info.h); the sim is not a board, and saying so
  // is the point — a default of "unknown" here would look like a bug in the panel it is reviewing.
  ui_set_device_info(
      DeviceInfo{"simulator", "sim", panel::kPortrait ? "320x480 portrait" : "320x240 landscape"});

  // The stepper demo's view model must outlive the action loop (the widgets bind to its
  // subject), so it lives at function scope.
  ValueStepperViewModel stepper_vm;
  NumericKeypadViewModel keypad_vm;
  SelectableListModel list_model;
  SimSettingsStorage sim_storage;
  SettingsStore settings_store(sim_storage); // CYD-side store the Settings screen edits (local)
  SettingsScreen settings;

  // The fake remote stack (Wave R3b): the profile library + editor are clients of the controller's
  // ManagementResponder now, so the sim stands up BOTH ends over an in-process LoopbackPipe — the
  // same object graph test_management_roundtrip builds — and pump_link()/settle() drives it.
  LoopbackPipe pipe;
  FakeClock clk;
  SimProfileStorage cure_storage;
  SimProfileStorage reflow_storage;
  SimSettingsStorage ctrl_settings_storage;
  control::ProfileStore cure_profiles(cure_storage, oven_Mode_MODE_CURE);
  control::ProfileStore reflow_profiles(reflow_storage, oven_Mode_MODE_REFLOW);
  control::SettingsStore ctrl_settings(ctrl_settings_storage);
  protocol::MessageRouter ctrl_router;
  protocol::FrameLink ctrl_link(pipe.b(), TF_SLAVE, ctrl_router);
  ManagementResponder responder(ctrl_link, cure_profiles, reflow_profiles);
  protocol::MessageRouter cyd_router;
  protocol::FrameLink cyd_link(pipe.a(), TF_MASTER, cyd_router);
  ManagementClient client(cyd_link, clk);
  responder.setSettingsStore(ctrl_settings);
  ctrl_router.setObserver(responder);
  cyd_router.setObserver(client);

  ProfileLibraryScreen profiles;
  ProfileEditorScreen editor;
  if (screen == "settings") {
    // The full Settings hub over an in-memory store (defaults). Navigate with click actions.
    settings_store.load();
    settings.begin(lv_screen_active(), settings_store);
  } else if (screen == "list") {
    // A settings-hub-shaped list (§24) with a disabled "coming soon" row, to review the
    // ▲/▼-highlight + Open layout without a hosting Settings screen.
    static const SelectableListItem items[] = {
        {"Display & units", nullptr, true},
        {"Temperature limits", nullptr, true},
        {"Network (WiFi)", "soon", false},
        {"About", nullptr, true},
    };
    // Count derived from the array, not written twice: deleting a row above used to leave a
    // stale literal here reading past the end.
    list_model.init(items, static_cast<int>(sizeof(items) / sizeof(items[0])));
    create_selectable_list(lv_screen_active(), list_model);
  } else if (screen == "stepper") {
    // A representative nudge-range field: idle timeout 1–10 min, default 2 (§24).
    stepper_vm.init(NumericFieldConfig{1, 10, 1, 2, "min", nullptr}, 2);
    create_value_stepper(lv_screen_active(), stepper_vm, "Idle timeout");
  } else if (screen == "keypad") {
    // A representative wide-range field (fails the >20-step rule → keypad, §24/§26): the UV
    // temp cap, 60–250 °C, default 100.
    keypad_vm.init(NumericFieldConfig{60, 250, 1, 100, "°C", nullptr}, 100);
    create_numeric_keypad(lv_screen_active(), keypad_vm, "Target temp");
  } else if (screen == "alerts") {
    build_alert_specimen(lv_screen_active());
  } else if (screen == "curve") {
    build_curve_demo(lv_screen_active());
  } else if (screen == "profile-library") {
    // The full §23 library over seeded in-memory stores. `--screen profile-library` opens the
    // Cure|Reflow chooser; click into a mode, highlight a row, Open for the detail/curve. A stock
    // profile (🔒) shows the read-only gating (Delete disabled, Edit → Save as).
    Phase lf245[4] = {};
    lf245[0].targetC = 150.0f;
    lf245[0].rampSeconds = 90.0f;
    lf245[0].holdSeconds = 90.0f;
    lf245[1].targetC = 180.0f;
    lf245[1].rampSeconds = 60.0f;
    lf245[1].holdSeconds = 60.0f;
    lf245[2].targetC = 245.0f;
    lf245[2].rampSeconds = 35.0f;
    lf245[2].holdSeconds = 30.0f;
    lf245[3].targetC = 50.0f;
    Phase sac305[3] = {};
    sac305[0].targetC = 165.0f;
    sac305[0].rampSeconds = 100.0f;
    sac305[0].holdSeconds = 90.0f;
    sac305[1].targetC = 249.0f;
    sac305[1].rampSeconds = 40.0f;
    sac305[1].holdSeconds = 30.0f;
    sac305[2].targetC = 50.0f;
    seed_profile(reflow_profiles, RecipeMode::Reflow, "LF-245", /*stock=*/false, lf245, 4);
    seed_profile(reflow_profiles, RecipeMode::Reflow, "SAC305", /*stock=*/true, sac305, 3);
    // A cure profile so the chooser's other library is not empty.
    Phase cure[2] = {};
    cure[0].targetC = 60.0f;
    cure[0].rampSeconds = 0.0f;
    cure[0].uv = true;
    cure[0].motor = true;
    cure[0].exposurePerSurface = 45.0f;
    cure[1].targetC = 40.0f;
    seed_profile(cure_profiles, RecipeMode::Cure, "Resin-A", /*stock=*/false, cure, 2);
    // LINK_OK so the (now link-gated) chooser tiles are navigable; pass `link none` as an action to
    // review the disconnected banner + greyed buttons. pump_link()/settle() drives the round-trips.
    lv_subject_set_int(&subj_link_state, LINK_OK);
    profiles.begin(lv_screen_active(), client);
    g_link_cyd = &cyd_link;
    g_link_ctrl = &ctrl_link;
    g_client = &client;
    g_profiles_screen = &profiles;
  } else if (screen == "editor" || screen == "editor-cure") {
    // The §12 profile editor on a fresh template. Overview first (curve + phase rows + Save); click
    // a phase row's Edit to drill into its field list. `--screen editor-cure` seeds a cure profile
    // so the UV shading + UV/Turntable rows show; `--screen editor` a reflow one.
    const bool cure = screen == "editor-cure";
    const RecipeMode m = cure ? RecipeMode::Cure : RecipeMode::Reflow;
    // A fresh template (a new profile), so this path is synchronous — no fetch. Save commits over
    // the link, so LINK_OK keeps its button live; `link none` demos the greyed Save + banner.
    lv_subject_set_int(&subj_link_state, LINK_OK);
    editor.beginNew(profile_templates::defaultTemplate(m), client, /*saveAs=*/true);
    editor.render(lv_screen_active());
    g_link_cyd = &cyd_link;
    g_link_ctrl = &ctrl_link;
    g_client = &client;
    g_editor_screen = &editor;
  } else if (screen == "home") {
    create_home_screen(lv_screen_active());
  } else {
    std::fprintf(stderr, "Unknown screen: %s\n", screen.c_str());
    return usage(argv[0]);
  }
  lv_obj_update_layout(lv_screen_active());
  settle(); // creation-time timers/animations (the old fixed 50 ms was under the 80 ms transition)

  size_t i = 0;
  auto take2 = [&](int32_t *x, int32_t *y) {
    return i + 2 <= tokens.size() && parse_i32(tokens[i].c_str(), x) &&
           parse_i32(tokens[i + 1].c_str(), y) && (i += 2, true);
  };

  while (i < tokens.size()) {
    const std::string op = tokens[i++];
    int32_t x = 0, y = 0;
    if (op == "click") {
      if (!take2(&x, &y))
        return usage(argv[0]);
      lv_test_mouse_click_at(x, y);
    } else if (op == "press") {
      if (!take2(&x, &y))
        return usage(argv[0]);
      lv_test_mouse_move_to(x, y);
      lv_test_mouse_press();
      lv_test_wait(50);
    } else if (op == "moveto") {
      if (!take2(&x, &y))
        return usage(argv[0]);
      lv_test_mouse_move_to(x, y);
      lv_test_wait(50);
    } else if (op == "release") {
      lv_test_mouse_release();
      lv_test_wait(50);
    } else if (op == "wait") {
      if (i >= tokens.size() || !parse_i32(tokens[i++].c_str(), &x))
        return usage(argv[0]);
      lv_test_wait(static_cast<uint32_t>(x));
    } else if (op == "shot") {
      if (i >= tokens.size())
        return usage(argv[0]);
      if (!write_png(tokens[i++].c_str()))
        return 2;
    } else if (op == "frame") {
      // Unsettled capture — see write_png. For photographing animation only.
      if (i >= tokens.size())
        return usage(argv[0]);
      if (!write_png(tokens[i++].c_str(), false))
        return 2;
    } else if (op == "temp") {
      if (i >= tokens.size() || !parse_i32(tokens[i++].c_str(), &x))
        return usage(argv[0]);
      lv_subject_set_int(&subj_chamber_temp, x);
    } else if (op == "state") {
      if (i >= tokens.size())
        return usage(argv[0]);
      const std::string v = tokens[i++];
      int s = v == "idle"      ? RUN_IDLE
              : v == "hot"     ? RUN_HOT
              : v == "running" ? RUN_RUNNING
              : v == "fault"   ? RUN_FAULT
                               : -1;
      if (s < 0) {
        std::fprintf(stderr, "Unknown state: %s\n", v.c_str());
        return usage(argv[0]);
      }
      lv_subject_set_int(&subj_run_state, s);
    } else if (op == "sensor") {
      // Board capability, as data: `sensor off` is how a board with no LDR reaches the UI
      // (subj_has_ambient_light). Without this the sim could only ever render the fitted case, so
      // the sensorless Settings rows — a disabled "Not fitted" auto row and an absolute Screen
      // brightness in place of the bias — could not be reviewed at all. A panel reads this when it
      // is BUILT, so put `sensor off` before the clicks that open the panel you want to see.
      if (i >= tokens.size())
        return usage(argv[0]);
      const std::string v = tokens[i++];
      if (v != "on" && v != "off") {
        std::fprintf(stderr, "Unknown sensor: %s (want on|off)\n", v.c_str());
        return usage(argv[0]);
      }
      lv_subject_set_int(&subj_has_ambient_light, v == "on" ? 1 : 0);
    } else if (op == "link") {
      if (i >= tokens.size())
        return usage(argv[0]);
      const std::string v = tokens[i++];
      int s = v == "ok" ? LINK_OK : v == "none" ? LINK_NONE : v == "schema" ? LINK_SCHEMA : -1;
      if (s < 0) {
        std::fprintf(stderr, "Unknown link: %s\n", v.c_str());
        return usage(argv[0]);
      }
      lv_subject_set_int(&subj_link_state, s);
    } else {
      std::fprintf(stderr, "Unknown action: %s\n", op.c_str());
      return usage(argv[0]);
    }
  }

  return write_png(out_path) ? 0 : 2;
}
