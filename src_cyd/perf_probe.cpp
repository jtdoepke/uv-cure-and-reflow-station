#ifdef PERF_PROBE

#include "perf_probe.h"

#include <Arduino.h>
#include <esp_timer.h>
#include <lvgl.h>

#include "perf_stats.h"

namespace perf_probe {
namespace {

NavHooks g_nav{};

// Populated by note_flush/note_endwrite during a burst, read after each refresh. `active` gates
// them so the millions of steady-state flushes between bursts pay nothing.
bool g_active = false;
int64_t g_flush_sum_us = 0;
int64_t g_endwrite_sum_us = 0;
uint32_t g_chunks = 0;

void span_reset() {
  g_flush_sum_us = 0;
  g_endwrite_sum_us = 0;
  g_chunks = 0;
}

// One timed full-screen redraw of the active screen. Returns total refresh microseconds; the
// per-refresh flush/endwrite/chunks land in the globals above.
//
// esp_timer_get_time() is microseconds — millis() would quantise ~1 ms chunks into uselessness.
int64_t timed_refresh() {
  span_reset();
  lv_obj_invalidate(lv_screen_active());
  int64_t t0 = esp_timer_get_time();
  lv_refr_now(nullptr);
  return esp_timer_get_time() - t0;
}

// A steady-state redraw burst on whatever screen is showing. Warm up (first refresh pays the
// glyph cache and a cold flash-cache), then N measured.
//
// Reports the CPU/SPI split. render = total - flush_sum: with double buffering the DMA OVERLAPS
// render, so this is "CPU render, DMA-overlapped", not a clean subtraction — the -D
// DISP_DOUBLE_BUFFER=0 build makes the flush blocking and turns flush_sum into true SPI wall
// time. The two builds bracket the truth; the delta between them is how much overlap the second
// buffer is actually buying.
void burst(const char *label, int iters) {
  perf::Samples total, flush, endwrite;
  uint32_t chunks = 0;

  for (int i = 0; i < iters + 3; ++i) {
    int64_t t = timed_refresh();
    if (i >= 3) {
      total.record(static_cast<uint32_t>(t));
      flush.record(static_cast<uint32_t>(g_flush_sum_us));
      endwrite.record(static_cast<uint32_t>(g_endwrite_sum_us));
      chunks = g_chunks; // constant across a screen; last one wins
    }
  }

  int64_t render = static_cast<int64_t>(total.median()) - static_cast<int64_t>(flush.median());
  if (render < 0) {
    render = 0;
  }

  Serial.printf("[perf] %s\trefr_total_us\t%u\n", label, total.median());
  Serial.printf("[perf] %s\trefr_total_p95_us\t%u\n", label, total.percentile(95));
  Serial.printf("[perf] %s\tflush_sum_us\t%u\n", label, flush.median());
  Serial.printf("[perf] %s\tendwrite_us\t%u\n", label, endwrite.median());
  Serial.printf("[perf] %s\trender_us\t%lld\n", label, static_cast<long long>(render));
  Serial.printf("[perf] %s\tchunks\t%u\n", label, chunks);
  Serial.printf("[perf] %s\tsamples\t%u\n", label, static_cast<unsigned>(total.count()));
}

// Home <-> Settings round trips, reporting the Home redraw cost of each trip. RISING = the
// dot-grid leak is live on real glass (grid_draw_cb accumulates on the persistent screen across
// lv_obj_clean). This is the on-hardware version of the host's leak_regression, and the number
// that says whether fixing it is felt by a hand on the panel.
void nav_burst(int trips) {
  if (g_nav.to_settings == nullptr || g_nav.to_home == nullptr) {
    Serial.println("[perf] nav hooks not set");
    return;
  }
  for (int i = 0; i < trips; ++i) {
    g_nav.to_settings();
    lv_refr_now(nullptr);
    g_nav.to_home();
    int64_t t = timed_refresh();
    Serial.printf("[perf] nav\ttrip_%02d_home_us\t%lld\n", i, static_cast<long long>(t));
  }
}

void run(char cmd) {
  // The burst BLOCKS loop(). That is deliberate: while it runs, the 1 Hz [ldr]/[link] serial
  // traces and the auto-brightness ADC tick in loop() do not fire, so they cannot contaminate
  // the numbers. ~160 chars of blocking Serial.printf at 115200 is ~14 ms — larger than several
  // of the effects being hunted.
  g_active = true;
  switch (cmd) {
  case 'h':
    burst("home", 30);
    break;
  case 'n':
    nav_burst(12);
    break;
  case 'a':
    burst("home", 30);
    nav_burst(12);
    break;
  default:
    Serial.println("[perf] cmds: h=home redraw  n=nav round-trips  a=all");
    break;
  }
  g_active = false;
  Serial.println("[perf] done");
}

} // namespace

void begin(const NavHooks &hooks) {
  g_nav = hooks;
  Serial.println("[perf] probe ready — send h (home) / n (nav) / a (all)");
}

void note_flush(int64_t us) {
  if (!g_active) {
    return;
  }
  g_flush_sum_us += us;
  ++g_chunks;
}

void note_endwrite(int64_t us) {
  if (!g_active) {
    return;
  }
  g_endwrite_sum_us += us;
}

void service() {
  if (Serial.available() <= 0) {
    return;
  }
  int c = Serial.read();
  if (c == '\n' || c == '\r') {
    return;
  }
  run(static_cast<char>(c));
}

} // namespace perf_probe

#endif // PERF_PROBE
