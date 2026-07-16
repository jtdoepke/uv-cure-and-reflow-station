// perf_probe — on-glass performance instrumentation, compiled only under -D PERF_PROBE=1
// (env esp32dev_cyd35_perf). The device half of the harness in perf/perf_main.cpp: the host
// measures CPU render cost with no SPI, this measures what the SPI bus actually costs and
// separates it from render, which is the number that decides whether an optimisation is worth
// anything on this board.
//
// Guarded like ui_dev_tools: production firmware never links it. Lives in src_cyd/ (not lib/)
// because it drives the real LGFX flush and the screen the firmware owns — but the STATISTICS
// come from lib/perf/perf_stats, shared with the host tool so a host number and a board number
// are computed identically.
#pragma once

#ifdef PERF_PROBE

#include <cstdint>

namespace perf_probe {

// How the probe drives a Home <-> Settings round trip without reaching into main.cpp's static
// navigation functions. main.cpp supplies these at begin(); the round trip is the on-glass
// reproduction of the dot-grid leak (each Home rebuild should NOT get slower once fixed).
struct NavHooks {
  void (*to_settings)();
  void (*to_home)();
};

void begin(const NavHooks &hooks);

// --- Flush-path instrumentation, called from my_disp_flush -----------------------------------
//
// note_flush() is called once per chunk with the microseconds spent in the whole flush call;
// note_endwrite() adds the time inside gfx.endWrite() (the final DMA drain). Both no-op unless a
// measurement burst is running, so the steady-state cost is one comparison.
void note_flush(int64_t us);
void note_endwrite(int64_t us);

// Read a one-char command off Serial and, if present, run the matching measurement burst
// (blocking — see the .cpp for why that is the point). Call once per loop().
void service();

} // namespace perf_probe

#endif // PERF_PROBE
