# Device baseline — 3.5" ST7796 (esp32dev_cyd35), captured 2026-07-16

On-glass, via `src_cyd/perf_probe.cpp` (env `esp32dev_cyd35_perf`), 30-iteration bursts after
3 warmup, `esp_timer_get_time()` microseconds. Median unless noted. This is the ground truth the
host harness (`perf/baseline/35.tsv`) only proxies.

## The headline: this board is RENDER-bound, not SPI-bound

Full Home redraw, production config (double-buffered):

| metric | µs | what it is |
|---|--:|---|
| refr_total | 144185 | whole `lv_refr_now` |
| render (total − flush_sum) | 138037 | **CPU compositing** |
| flush_sum | 6148 | async DMA enqueue + buffer-reuse stalls |
| endwrite | 3117 | final DMA drain |
| chunks | 20 | = 480 / DRAW_BUF_LINES(24), confirms PARTIAL path |

Single-buffer calibration (`esp32dev_cyd35_perf_sb`, blocking flush → flush_sum is TRUE SPI wall
time):

| metric | µs |
|---|--:|
| refr_total | 190332 |
| flush_sum (SPI wall) | 66661 |
| render (CPU) | 123671 |

**The split, reconciled:** CPU render ≈ 124–138 ms, SPI wall ≈ 67 ms. Double-buffering overlaps
the SPI under the render, so the production redraw (144 ms) is gated by RENDER, and the second
15 KB draw buffer saves 190 − 144 = **46 ms per redraw (24%)** — it earns its DRAM; do not drop it.

### The grid is NOT the bottleneck (measured, 2026-07-16)

Task count made the dot grid look like the villain — ~600 of a Home redraw's ~900
draw tasks. It is not. Building with grid_draw_cb short-circuited to an immediate return
(diagnostic `-D PERF_NO_GRID`, never committed enabled):

| | Home render |
|---|--:|
| grid on | 129 ms |
| grid off | 116 ms |

The entire dot grid costs **~13 ms (10%)**. So render is **not task-count-bound** — 600
single-pixel fills are 600 pixels, trivial pixel volume however many tasks they are. The
other ~116 ms is real compositing: the full-screen opaque background, the `LV_OPA_50` mode
tiles that alpha-blend against the canvas beneath them (the design's see-through tiles —
"every press forces the canvas to recomposite", design.md), and text glyphs. All of it is
pixel-locked by the design.

**Consequence:** a batched or tiled-image grid (the deferred high-risk item) would save at
most that 13 ms for real pixel-diff risk. Dropped. The render floor is design-inherent, and
the extractable render wins were the leak (unbounded, now fixed) and the ~200 off-clip dots.

### Full render decomposition (measured 2026-07-16, diagnostic builds)

Home full redraw isolated by short-circuiting each element (`-D PERF_NO_GRID` / `PERF_OPAQUE`
/ `PERF_NO_TEXT`, and the probe's `b` blank-screen command — none committed enabled):

| element | cost | share |
|---|--:|--:|
| panel / tile / border structure fills | ~56 ms | 43% |
| text glyphs (big anti-aliased readout + tile fonts) | ~35 ms | 27% |
| full-screen background fill | ~26 ms | 20% |
| dot grid | ~13 ms | 10% |
| (of which alpha blending, translucent tiles + hairlines) | ~11 ms | — |

### No bit-twiddling headroom left in the pixel loops (researched 2026-07-16)

Checked whether SWAR / clever-blend tricks could speed the per-pixel math. They can't — LVGL 9.5
already implements all of them in `lv_draw_sw_blend_to_rgb565_swapped.c`:

- opaque fill: 32-bit stores (2 px/write), 8× unrolled (16 px/iteration), with alignment handling.
- opaque copy: `lv_memcpy`.
- mask/text (A4 glyph) blend: 2-px unrolled, reads the mask 16 bits at a time, and **skips
  fully-opaque runs** (`mask16==0xFFFF` → straight copy) so the multiply-mix runs only on the thin
  AA edges.

Even the SWAR two-pixel *blend* is already in: `lv_color_16_16_mix` (misc/lv_color.c, called by the
mask/text and WITH_OPA paths) IS the `0x7E0F81F` packed-mask, two-channels-in-one-32-bit-register,
single-multiply-per-pair trick — 5-bit-quantized alpha, `((fg-bg)*mix)>>5)+bg`. So lvgl#5015's core
idea is mainline. The only unmerged part of #5015 was replacing that one multiply with a 33-case
lookup, and THAT variant rounds differently (its output is off by ±1 LSB on some inputs vs the
current `*mix>>5`), so it is not pixel-identical — it would fail the byte-exact golden gate for a
micro-gain on a multiply that only runs on thin glyph edges. A pixel-safe alpha LUT for text was BUILT
AND MEASURED (2026-07-16) — and it is a LOSS. Ceiling first: hooking the swapped WITH_MASK path to
skip the mix entirely dropped Home render 129 → 113 ms, so the glyph-edge mix is ~16 ms. But the
real pixel-identical LUT (self-adapting cache keyed on fg/bg, each entry computed with the exact
`lv_color_swap_16`/`lv_color_16_16_mix`, per-pixel fallback for varying bg) measured **132.7 ms —
WORSE than the 129 ms baseline**. The cache management alone (per-pixel bg compare + generation
check + array loads that stall on cache misses) costs ~20 ms, more than the 16 ms mix it replaces,
and the per-pixel loop also loses LVGL's 2-px-batched interior fast path. The lesson: `mix` is
already a single SWAR multiply, and on the LX6 a *correct* table lookup cannot beat a single
multiply. The 16 ms ceiling was real but misleading — removing work ≠ a cheaper replacement
existing. Spike reverted. **None of these are worth it: the per-pixel math is not the bottleneck,
and it is already optimal.**

Render is **compositing-bound**, and specifically DISPATCH/per-chunk-overhead-bound, not
pixel-math-bound: the DRAW_BUF_LINES sweep cut render 34% with identical pixels, i.e. ~44 ms was
pure per-chunk overhead in LVGL 9's draw-task pipeline (a ~37% v8→v9 regression is reported
upstream, lvgl#5459, closed not-planned). Each element is also ~40 cycles/pixel because the SW
renderer runs unaccelerated from flash (LV_USE_DRAW_SW_ASM is NONE — no Xtensa backend that
helps — and IRAM placement overflows). None of the four has a pixel-safe cheaper path; each is
core to the design. The one systemic lever that cuts across all of them without touching pixels
is **fewer, taller chunks** (DRAW_BUF_LINES) — a widget crossing chunks is re-drawn per chunk,
so taller buffers cut the whole table proportionally. That is the documented deferred win
(cyd_board.h), gated on the OTA DRAM budget.

### Dual-core rendering (LV_DRAW_SW_DRAW_UNIT_CNT=2 + LV_USE_OS=FREERTOS): measured, a LOSS (2026-07-16)

LVGL 9's threaded software draw units are the obvious "use the second core" lever. Tried it on
glass: `LV_USE_OS=LV_OS_FREERTOS` + `LV_DRAW_SW_DRAW_UNIT_CNT=2`. Two gotchas first — (1) editing
only `lv_conf.h` does NOT recompile LVGL's library objects in PlatformIO (they aren't tracked as
depending on `lv_conf.h`; a first "dual-core" build silently reused the stale single-unit objects
and reported a bit-identical baseline number — a false null). A `pio run -t clean` of the env is
mandatory. (2) On a clean build `lv_freertos.c` won't compile: it `#include "atomic.h"` bare, and
ESP-IDF's FreeRTOS atomic header is `freertos/atomic.h` (not on the default path) — needs the
include qualified or the dir added.

With a genuinely fresh dual-core build (`tasks` 7→9, free_heap −19 kB for two 8 kB worker stacks):

| build | Home render | vs baseline |
|---|--:|--:|
| 1 draw unit (production) | 129.6 ms | — |
| **2 draw units, FreeRTOS** | **229.0 ms** | **+77% SLOWER** |

A per-worker task counter instrumented into `render_thread_cb` shows why, decisively: over 30
refreshes **worker 0 ran 30338 tasks, worker 1 ran 253 — 0.8%**. Worker 1 is starved. LVGL only
hands a task to a second unit if it is `is_independent` — no older *unfinished* task overlaps its
area (`lv_draw.c`). Our UI is a deep composite: a full-chunk background fill is task 0 of every
chunk, then panels → translucent LV_OPA_50 tiles → the screen-wide dot grid → text, each drawn ON
TOP of and overlapping everything beneath. While the background renders, every other task overlaps
it, so nothing is independent; even after it, the panels/tiles/dots/text mostly overlap each other.
The layered depth that gives the UI its look collapses the draw-task dependency graph into a chain,
not a fan — there is almost nothing to parallelise. On top of getting ~no parallelism, the threaded
dispatch replaces ~1000 inline `execute_drawing()` calls per frame with FreeRTOS semaphore
round-trips (~98 µs/task of pure sync overhead) — that is the +100 ms. **Core-pinning cannot save
it:** worker 1 has no work to place on the other core; the starvation is the dependency graph, not
core contention. Spike reverted (both `lv_conf.h` values and the two vendored patches). Dead end,
now with numbers.

### Consequences for the candidate list

- **SPI 40→80 MHz (Opt-12/13): deprioritised to last-resort.** SPI is already fully hidden under
  render; halving it moves the render-bound total by ~10% at most, for a corruption risk that has
  no on-glass detector (dev-shot is 501 here). Not worth it until render is much smaller.

- **esp_lvgl_port Xtensa ASM blenders (LV_DRAW_SW_ASM_CUSTOM): measured DEAD END on this chip.**
  esp-bsp does ship classic-ESP32 `_esp32.S` fill/copy blenders (not only S3), and they wire in
  cleanly via the CUSTOM hook (vendored + adapted for 9.5.0's descriptor typedef rename and its
  wider CUSTOM_INCLUDE fan-out). But on glass they gave **nothing**: plain-RGB565 + C = 120.8 ms,
  plain-RGB565 + ASM = 121.1 ms (identical). The fill asm fires (no early bail) but cannot beat
  the C 16-bit replication loop on the LX6, which has no SIMD — the vendor's 5.8×/9.8× figures are
  ESP32-**S3** (real vector ops). And it only ever hooks the OPAQUE fill/copy paths, not the
  text/alpha blends that are 38% of our render. Spike reverted.
  - **Incidental finding, then CLOSED:** plain RGB565 renders ~8 ms (6%) faster than
    RGB565_SWAPPED (120.8 vs ~129 ms) — the swapped path byte-swaps per pixel during compositing.
    Tempting to render plain and swap in the flush. **Measured: it is a LOSS.** Plain render +
    `gfx.pushImage(rgb565_t)` (LovyanGFX's pixelcopy-swap-into-DMA path) gives refr_total **188 ms
    vs the swapped path's 136 ms** — flush_sum explodes to 62 ms because pushImage's per-chunk
    CPU swap is effectively blocking and does NOT overlap the next chunk's render, unlike the
    current async zero-copy `pushImageDMA` session. The byte-swap is ~8 ms of unavoidable CPU work
    (the ESP32 SPI has no clean 16-bit hardware byte-swap — `WR_BYTE_ORDER` reorders 32-bit words,
    i.e. pixel pairs, and LovyanGFX never uses it), and the current design already places it
    optimally: during render, with zero-copy async DMA. Don't revisit unless the swap can be
    offloaded to core 0 (dual-core rendering), which is a separate architecture.
- **The dot grid is the prize.** The host harness shows it is ~80% of Home's draw tasks, render is
  80–90% of the redraw, so cutting grid tasks cuts the thing that actually dominates.
- **`DISP_DOUBLE_BUFFER=0` to reclaim 15 KB: costs 46 ms/redraw.** Only if RAM-starved.

## The leak, on real glass (`n` = Home↔Settings round trips)

Home redraw time per trip — RISES because `grid_draw_cb` accumulates on the persistent screen
across `lv_obj_clean` (see the Phase-0 commit):

| trip | 0 | 1 | 2 | … | 11 |
|---|--:|--:|--:|--:|--:|
| Home redraw µs | 199406 | 219137 | 239002 | … | 417216 |

~+20 ms per navigation, exactly linear. After a dozen screens the panel takes **0.42 s** to
repaint a single frame. This is the same defect the host `leak_regression` reports as +800
tasks/visit; here it is felt directly by a hand on the glass. `tasks_growth` must reach 0 after
the fix, and Home redraw must return to the flat ~144 ms.
