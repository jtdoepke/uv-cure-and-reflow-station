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
