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

Render is **compositing-bound**: it is drawing the design's elements, each at ~40 cycles/pixel
because the SW renderer runs unaccelerated from flash (LV_USE_DRAW_SW_ASM is NONE — no Xtensa
backend — and IRAM placement overflows). None of the four has a pixel-safe cheaper path; each is
core to the design. The one systemic lever that cuts across all of them without touching pixels
is **fewer, taller chunks** (DRAW_BUF_LINES) — a widget crossing chunks is re-drawn per chunk,
so taller buffers cut the whole table proportionally. That is the documented deferred win
(cyd_board.h), gated on the OTA DRAM budget.

### Consequences for the candidate list

- **SPI 40→80 MHz (Opt-12/13): deprioritised to last-resort.** SPI is already fully hidden under
  render; halving it moves the render-bound total by ~10% at most, for a corruption risk that has
  no on-glass detector (dev-shot is 501 here). Not worth it until render is much smaller.
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
