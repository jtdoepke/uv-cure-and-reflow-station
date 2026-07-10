// Headless UI simulator (env native_sim) — the Claude-assisted UI development loop.
//
// Renders the real lib/ui_logic widgets on LVGL's in-memory test display (same RGB565
// rasterizer as the firmware), optionally injects scripted pointer events, and writes
// PNG screenshots. No display server, no hardware. See the ui-development skill.
//
// Usage: program [--out PATH] [ACTION...]
//   click X Y | press X Y | moveto X Y | release | wait MS | shot PATH
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

#include "main_ui.h"

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
               "Usage: %s [--out PATH] [ACTION...]\n"
               "Actions: click X Y | press X Y | moveto X Y | release | wait MS | shot PATH\n",
               argv0);
  return 1;
}

int main(int argc, char **argv) {
  const char *out_path = ".pio/sim/ui.png";
  std::vector<std::string> tokens;
  for (int i = 1; i < argc; i++) {
    if (std::strcmp(argv[i], "--out") == 0) {
      if (i + 1 >= argc)
        return usage(argv[0]);
      out_path = argv[++i];
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
  create_main_ui(lv_screen_active());
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
    } else {
      std::fprintf(stderr, "Unknown action: %s\n", op.c_str());
      return usage(argv[0]);
    }
  }

  return write_png(out_path) ? 0 : 2;
}
