// Headless UI simulator (env native_sim) — the Claude-assisted UI development loop.
//
// Renders the real lib/ui_logic widgets on LVGL's in-memory test display (same RGB565
// rasterizer as the firmware), optionally injects scripted pointer events, and writes
// PNG screenshots. No display server, no hardware. See the ui-development skill.
//
// Usage: program [--out PATH] [--screen home|stepper|keypad|list|settings] [ACTION...]
//   click X Y | press X Y | moveto X Y | release | wait MS | shot PATH
//   temp N | state idle|hot|running|fault | link ok|none|schema
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
#include <string>
#include <vector>

#include "home_screen.h"
#include "numeric_keypad.h"
#include "selectable_list.h"
#include "settings_screen.h"
#include "subjects.h"
#include "value_stepper.h"

// Non-persistent in-memory storage so the sim can build a SettingsStore (layout review only —
// nothing is saved across runs).
struct SimSettingsStorage : ISettingsStorage {
  size_t load(uint8_t *, size_t) override { return 0; }
  bool save(const uint8_t *, size_t) override { return true; }
};

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

static const int32_t SCR_W = 320, SCR_H = 240; // landscape, same as the firmware
static lv_display_t *sim_disp = nullptr;

// Render pending changes, then encode the display's full-frame buffer as a 24-bit PNG.
// The test display runs in DIRECT render mode; we set its color format to RGB565 so the
// rasterizer output matches the firmware exactly, and expand to RGB888 only here.
static bool write_png(const char *path) {
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
               "Usage: %s [--out PATH] [--screen home|stepper|keypad|list|settings] [ACTION...]\n"
               "Actions: click X Y | press X Y | moveto X Y | release | wait MS | shot PATH\n"
               "         temp N | state idle|hot|running|fault | link ok|none|schema\n",
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
  sim_disp = lv_test_display_create(SCR_W, SCR_H);
  // The test display defaults to XRGB8888; render RGB565 like the firmware instead
  // (the display reallocates its full-frame buffer on this event).
  lv_display_set_color_format(sim_disp, LV_COLOR_FORMAT_RGB565);
  lv_test_indev_create_all();
  ui_subjects_init();

  // The stepper demo's view model must outlive the action loop (the widgets bind to its
  // subject), so it lives at function scope.
  ValueStepperViewModel stepper_vm;
  NumericKeypadViewModel keypad_vm;
  SelectableListModel list_model;
  SimSettingsStorage sim_storage;
  SettingsStore settings_store(sim_storage);
  SettingsScreen settings;
  if (screen == "settings") {
    // The full Settings hub over an in-memory store (defaults). Navigate with click actions.
    settings_store.load();
    settings.begin(lv_screen_active(), settings_store);
  } else if (screen == "list") {
    // A settings-hub-shaped list (§24) with a disabled "coming soon" row, to review the
    // ▲/▼-highlight + Open layout without a hosting Settings screen.
    static const SelectableListItem items[] = {
        {"Display & units", nullptr, true}, {"Temperature limits", nullptr, true},
        {"Sleep & wake", nullptr, true},    {"Network (WiFi)", "soon", false},
        {"About", nullptr, true},
    };
    list_model.init(items, 5);
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
  } else if (screen == "home") {
    create_home_screen(lv_screen_active());
  } else {
    std::fprintf(stderr, "Unknown screen: %s\n", screen.c_str());
    return usage(argv[0]);
  }
  lv_obj_update_layout(lv_screen_active());
  lv_test_wait(50); // let creation-time timers/animations settle

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
