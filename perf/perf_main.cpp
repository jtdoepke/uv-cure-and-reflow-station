// Headless UI performance harness (envs native_perf / native_perf_35).
//
// Renders the real lib/ui_logic screens on a display configured EXACTLY like the firmware's, times
// the render on the host CPU, and counts the draw tasks LVGL emits. No display server, no SPI, no
// hardware — so what it measures is pure composition cost.
//
// Usage: program --scenario NAME [--iters N]
//   Scenarios: home | settings | list | stepper | keypad | press | leak
// Emits TSV on stdout: a `#` provenance header, then `scenario<TAB>metric<TAB>value<TAB>unit`.
//
// ---------------------------------------------------------------------------------------------
// TWO THINGS THIS FILE IS CAREFUL ABOUT. Both are easy to get wrong and silently produce
// confident nonsense.
//
// 1. IT DOES NOT USE lv_test_display_create(), unlike sim/sim_main.cpp. That helper renders
//    LV_DISPLAY_RENDER_MODE_DIRECT into a full-frame buffer (lv_test_display.c). The firmware
//    renders LV_DISPLAY_RENDER_MODE_PARTIAL in DRAW_BUF_LINES-tall chunks, which means a screen's
//    LV_EVENT_DRAW_MAIN_END callback fires once PER CHUNK — ~20x per screen on the 3.5" panel,
//    but exactly ONCE on the test display. The per-chunk multiplication is the dominant cost on
//    this hardware, so a harness built on the test display would report the dot matrix as nearly
//    free and send you optimising the wrong thing. We therefore build the display by hand,
//    mirroring src_cyd/main.cpp's setup.
//
// 2. HOST NUMBERS ARE A DIRECTIONAL PROXY, NOT GROUND TRUTH. Host codegen, cache behaviour and
//    clock speed say nothing about Xtensa — and this harness has no SPI at all, where the device
//    does. The task COUNTS are exact and portable; the microseconds are for comparing a before
//    against an after ON THE SAME MACHINE. Never ship on a host win alone: confirm on glass
//    (src_cyd/perf_probe.cpp).
// ---------------------------------------------------------------------------------------------
#include <lvgl.h>
// The draw-unit struct is only fully defined in a private header. Acceptable here for the same
// reason sim_main.cpp includes an internal lv_test.h path: this is a host tool, not production
// UI. Nothing under lib/ or src_cyd/ may do this.
#include "src/draw/lv_draw_private.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/resource.h>

#include "device_info.h"
#include "home_screen.h"
#include "numeric_keypad.h"
#include "panel.h"
#include "perf_stats.h"
#include "selectable_list.h"
#include "settings_screen.h"
#include "subjects.h"
#include "theme.h"
#include "value_stepper.h"

namespace {

// Mirrors include/cyd_board.h:129 (DRAW_BUF_LINES = 24) and its kDrawBufBytes derivation. NOT
// included from there: cyd_board.h is board-aware and drags in LovyanGFX, and lib/-adjacent host
// code must stay board-blind (CLAUDE.md). Duplicated deliberately — keep in sync with that file;
// the sweep drives both through the same make variable.
#ifndef PERF_DRAW_BUF_LINES
#define PERF_DRAW_BUF_LINES 24
#endif
constexpr uint32_t kDrawBufBytes = panel::W * PERF_DRAW_BUF_LINES * 2;

// --- Instrumentation -------------------------------------------------------------------------

// Indexed by lv_draw_task_type_t. Sized past the largest enumerator (LV_DRAW_TASK_TYPE_3D) with
// room to spare; every write is bounds-checked, so an LVGL bump that adds a type cannot corrupt
// memory — it just lands in the tail bucket.
constexpr size_t kMaxTaskType = 32;
uint32_t g_task_counts[kMaxTaskType];
uint32_t g_chunks;

void reset_counters() {
  std::memset(g_task_counts, 0, sizeof(g_task_counts));
  g_chunks = 0;
}

uint32_t total_tasks() {
  uint32_t t = 0;
  for (size_t i = 0; i < kMaxTaskType; ++i) {
    t += g_task_counts[i];
  }
  return t;
}

// A draw unit that counts and nothing else.
//
// lv_draw_finalize_task_creation() calls evaluate_cb on EVERY registered unit, exactly once per
// task (lv_draw.c). By never touching task->preferred_draw_unit_id or ->preference_score, this
// unit leaves the software renderer to claim and draw every task exactly as it otherwise would —
// so counting is PIXEL-SAFE BY CONSTRUCTION, not merely believed to be.
//
// dispatch_cb must exist and return 0 ("took no tasks"); lv_draw_dispatch_layer calls it
// unconditionally and would otherwise dereference null.
int32_t count_evaluate(lv_draw_unit_t *, lv_draw_task_t *task) {
  size_t t = static_cast<size_t>(task->type);
  g_task_counts[t < kMaxTaskType ? t : kMaxTaskType - 1]++;
  return 0;
}

int32_t count_dispatch(lv_draw_unit_t *, lv_layer_t *) {
  return 0;
}

void install_counting_unit() {
  auto *u = static_cast<lv_draw_unit_t *>(lv_draw_create_unit(sizeof(lv_draw_unit_t)));
  u->name = "perf_count";
  u->evaluate_cb = count_evaluate;
  u->dispatch_cb = count_dispatch;
}

void perf_flush(lv_display_t *disp, const lv_area_t *, uint8_t *) {
  g_chunks++; // proves the PARTIAL path is live: a DIRECT display would report 1
  lv_display_flush_ready(disp);
}

// CPU time, not wall time: this must not measure a busy laptop. CLOCK_PROCESS_CPUTIME_ID counts
// only cycles this process actually ran, at ns resolution. (getrusage's time fields are
// tick-quantized and far too coarse; it is used below for ru_maxrss only.)
uint64_t cpu_now_us() {
  timespec ts{};
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000ull + static_cast<uint64_t>(ts.tv_nsec) / 1000;
}

// --- Reporting -------------------------------------------------------------------------------

const char *g_scenario = "";

void emit(const char *metric, uint64_t value, const char *unit) {
  std::printf("%s\t%s\t%llu\t%s\n", g_scenario, metric, static_cast<unsigned long long>(value),
              unit);
}

void emit_task_counts() {
  // Only the types that actually occur, so the table stays readable and a new one shows up rather
  // than hiding in a wall of zeroes.
  struct {
    lv_draw_task_type_t type;
    const char *name;
  } kNamed[] = {
      {LV_DRAW_TASK_TYPE_FILL, "tasks_fill"},   {LV_DRAW_TASK_TYPE_BORDER, "tasks_border"},
      {LV_DRAW_TASK_TYPE_LABEL, "tasks_label"}, {LV_DRAW_TASK_TYPE_LETTER, "tasks_letter"},
      {LV_DRAW_TASK_TYPE_IMAGE, "tasks_image"}, {LV_DRAW_TASK_TYPE_LAYER, "tasks_layer"},
      {LV_DRAW_TASK_TYPE_LINE, "tasks_line"},   {LV_DRAW_TASK_TYPE_ARC, "tasks_arc"},
  };
  emit("tasks_total", total_tasks(), "count");
  for (const auto &n : kNamed) {
    if (g_task_counts[n.type] != 0) {
      emit(n.name, g_task_counts[n.type], "count");
    }
  }
}

void emit_mem() {
  lv_mem_monitor_t mon;
  lv_mem_monitor(&mon);
  emit("lv_mem_used", mon.total_size - mon.free_size, "bytes");
  emit("lv_mem_max_used", mon.max_used, "bytes");
  emit("lv_mem_frag_pct", mon.frag_pct, "pct");

  rusage ru{};
  getrusage(RUSAGE_SELF, &ru);
  emit("rss_peak", static_cast<uint64_t>(ru.ru_maxrss), "kb");
}

// --- Screen construction ---------------------------------------------------------------------
//
// Function-scope so the view models outlive the widgets bound to their subjects (the sim has the
// same constraint).
ValueStepperViewModel g_stepper_vm;
NumericKeypadViewModel g_keypad_vm;
SelectableListModel g_list_model;

// Defaults-only, like the sim's: the perf numbers must not depend on a stored blob.
struct PerfSettingsStorage : ISettingsStorage {
  size_t load(uint8_t *, size_t) override { return 0; }
  bool save(const uint8_t *, size_t) override { return true; }
};
PerfSettingsStorage g_storage;
SettingsStore g_settings_store(g_storage);
SettingsScreen g_settings;

// The UV CURE tile, for the press scenario. Captured rather than guessed at by coordinate: a
// hardcoded point silently stops hitting anything the moment a layout token moves.
lv_obj_t *g_press_target = nullptr;

// Mirrors sim_main.cpp's demo fixtures so the two tools render the same widgets.
void build_screen(const std::string &screen, lv_obj_t *scr) {
  if (screen == "settings") {
    g_settings_store.load();
    g_settings.begin(scr, g_settings_store);
  } else if (screen == "list") {
    static const SelectableListItem items[] = {
        {"Display & units", nullptr, true},
        {"Temperature limits", nullptr, true},
        {"Network (WiFi)", "soon", false},
        {"About", nullptr, true},
    };
    g_list_model.init(items, static_cast<int>(sizeof(items) / sizeof(items[0])));
    create_selectable_list(scr, g_list_model);
  } else if (screen == "stepper") {
    g_stepper_vm.init(NumericFieldConfig{1, 10, 1, 2, "min", nullptr}, 2);
    create_value_stepper(scr, g_stepper_vm, "Idle timeout");
  } else if (screen == "keypad") {
    g_keypad_vm.init(NumericFieldConfig{60, 250, 1, 100, "°C", nullptr}, 100);
    create_numeric_keypad(scr, g_keypad_vm, "Target temp");
  } else {
    // A healthy link BEFORE building: the mode tiles bind LV_OBJ_FLAG_CLICKABLE to it (§9/§14
    // gate), and subjects boot at LINK_NONE. Without this the tiles are disabled, so the press
    // scenario would press a dead widget and measure an empty frame.
    lv_subject_set_int(&subj_link_state, LINK_OK);
    HomeScreen home = create_home_screen(scr);
    g_press_target = home.btn_cure;
  }
  lv_obj_update_layout(scr);
}

// Settle creation-time timers and the theme's 80 ms state transitions, so the first measured
// refresh is a steady-state redraw rather than a blend in progress.
void settle() {
  for (int i = 0; i < 40 && lv_anim_count_running() > 0; ++i) {
    lv_tick_inc(20);
    lv_timer_handler();
  }
  lv_tick_inc(20);
  lv_timer_handler();
}

// One full-screen redraw, timed. Invalidate + lv_refr_now is the honest full-repaint: it forces
// every chunk through the render path.
uint32_t timed_full_redraw(lv_obj_t *scr) {
  lv_obj_invalidate(scr);
  uint64_t t0 = cpu_now_us();
  lv_refr_now(nullptr);
  return static_cast<uint32_t>(cpu_now_us() - t0);
}

int usage(const char *argv0) {
  std::fprintf(stderr,
               "Usage: %s --scenario home|settings|list|stepper|keypad|press|leak"
               " [--iters N]\n",
               argv0);
  return 1;
}

} // namespace

int main(int argc, char **argv) {
  std::string scenario = "home";
  int iters = 50;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) {
      scenario = argv[++i];
    } else if (std::strcmp(argv[i], "--iters") == 0 && i + 1 < argc) {
      iters = std::atoi(argv[++i]);
    } else {
      return usage(argv[0]);
    }
  }
  if (iters <= 0 || static_cast<size_t>(iters) > perf::Samples::kCapacity) {
    std::fprintf(stderr, "--iters must be 1..%zu\n", perf::Samples::kCapacity);
    return 1;
  }
  g_scenario = scenario.c_str();

  lv_init();
  install_counting_unit();

  // The firmware's display, reproduced. See the file header for why this is not
  // lv_test_display_create().
  auto *buf1 = static_cast<uint8_t *>(std::malloc(kDrawBufBytes));
  auto *buf2 = static_cast<uint8_t *>(std::malloc(kDrawBufBytes));
  if (buf1 == nullptr || buf2 == nullptr) {
    std::fprintf(stderr, "FATAL: draw buffer allocation failed\n");
    return 1;
  }
  lv_display_t *disp = lv_display_create(panel::W, panel::H);
  lv_display_set_flush_cb(disp, perf_flush);
  lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
  lv_display_set_buffers(disp, buf1, buf2, kDrawBufBytes, LV_DISPLAY_RENDER_MODE_PARTIAL);

  ui_subjects_init();
  ui_set_device_info(
      DeviceInfo{"perf", "perf", panel::kPortrait ? "320x480 portrait" : "320x240 landscape"});

  std::printf("# schema=1\tpanel=%s\tres=%dx%d\tdraw_buf_lines=%d\tdraw_buf_bytes=%u\titers=%d\n",
              panel::kPortrait ? "35" : "28", static_cast<int>(panel::W),
              static_cast<int>(panel::H), PERF_DRAW_BUF_LINES, kDrawBufBytes, iters);
  std::printf("scenario\tmetric\tvalue\tunit\n");

  lv_obj_t *scr = lv_screen_active();

  // --- leak_regression -----------------------------------------------------------------------
  //
  // THE EXIT GATE for Phase 0. Reproduces exactly what firmware navigation does (main.cpp's
  // build_home: lv_obj_clean(lv_screen_active()) then create_home_screen(...)) and reports the
  // draw-task count of each rebuild's redraw.
  //
  // lv_obj_clean deletes only CHILDREN — never the screen object's own event list — so today
  // every rebuild leaves another grid_draw_cb attached and this series RISES without bound. Once
  // the attach is idempotent it must be FLAT. If this harness cannot see that, the harness is
  // wrong and nothing built on it can be trusted.
  if (scenario == "leak") {
    uint32_t first_tasks = 0, last_tasks = 0, first_cpu = 0, last_cpu = 0;
    for (int i = 0; i < iters; ++i) {
      lv_obj_clean(scr);
      create_home_screen(scr);
      lv_obj_update_layout(scr);
      settle();

      reset_counters();
      uint32_t us = timed_full_redraw(scr);
      uint32_t tasks = total_tasks();

      if (i == 0) {
        first_tasks = tasks;
        first_cpu = us;
      }
      last_tasks = tasks;
      last_cpu = us;

      char m[48];
      std::snprintf(m, sizeof(m), "rebuild_%02d_tasks", i);
      emit(m, tasks, "count");
    }

    // The summary CI gates on. `tasks_growth` is the whole point: a rebuild must cost the same as
    // the one before it, so this MUST be 0. Anything above zero means an event callback (or
    // anything else) is accumulating on the persistent screen object across lv_obj_clean.
    emit("first_tasks", first_tasks, "count");
    emit("last_tasks", last_tasks, "count");
    emit("tasks_growth", last_tasks - first_tasks, "count");
    emit("first_cpu", first_cpu, "us");
    emit("last_cpu", last_cpu, "us");
    emit_mem();
    return 0;
  }

  // --- Steady-state scenarios ----------------------------------------------------------------
  build_screen(scenario == "press" ? "home" : scenario, scr);
  settle();

  perf::Samples cpu;

  if (scenario == "press") {
    // The partial-invalidate cost of acknowledging a press — the render behind the design guide's
    // "visible reaction within 100 ms" rule, and the most common interaction on the panel.
    //
    // Driven by toggling LV_STATE_PRESSED directly rather than through lv_test_mouse_*, which
    // looks more realistic but cannot be timed honestly here: LVGL polls the input device from a
    // timer on LV_DEF_REFR_PERIOD, and the display's refresh timer is created FIRST, so a press
    // read during one lv_timer_handler() pass is not painted until the next. An earlier version
    // timed one handler call around a real click and measured an empty frame (0 chunks, ~1 us) —
    // it was reporting the scheduler, not the renderer.
    //
    // Setting the state is precisely what the indev read does, and it invalidates the same area,
    // so the repaint timed here is the real one. What it omits is timer scheduling and touch
    // latency — and those are DEVICE questions (LV_DEF_REFR_PERIOD, the shared-bus touch poll)
    // that belong to perf_probe, not to a host CPU benchmark that has neither.
    for (int i = 0; i < iters + 3; ++i) {
      lv_obj_add_state(g_press_target, LV_STATE_PRESSED);
      uint64_t t0 = cpu_now_us();
      lv_refr_now(nullptr);
      uint32_t us = static_cast<uint32_t>(cpu_now_us() - t0);
      if (i >= 3) {
        cpu.record(us);
      }
      lv_obj_remove_state(g_press_target, LV_STATE_PRESSED);
      settle(); // the theme's 120 ms release ease-out, so the next press starts from rest
      lv_refr_now(nullptr);
    }
  } else {
    for (int i = 0; i < iters + 3; ++i) {
      uint32_t us = timed_full_redraw(scr);
      if (i >= 3) { // discard warmup: the first refresh pays layout + the glyph cache
        cpu.record(us);
      }
    }
  }

  emit("cpu_median", cpu.median(), "us");
  emit("cpu_p05", cpu.percentile(5), "us");
  emit("cpu_p95", cpu.percentile(95), "us");
  emit("cpu_min", cpu.min(), "us");
  emit("samples", cpu.count(), "count");

  // Counts come from ONE further refresh, in isolation, so every count below is PER REFRESH.
  // Accumulating them across the timing loop above would report a 50x number that reads exactly
  // like a per-screen one.
  reset_counters();
  if (scenario == "press") {
    lv_obj_add_state(g_press_target, LV_STATE_PRESSED);
    lv_refr_now(nullptr);
    lv_obj_remove_state(g_press_target, LV_STATE_PRESSED);
  } else {
    timed_full_redraw(scr);
  }
  emit("chunks", g_chunks, "count");
  emit_task_counts();
  emit_mem();
  return 0;
}
