---
name: hardware-bringup
description: This skill should be used when the CYD display or touch hardware misbehaves — screen stays blank/white/garbled, colors look photo-negative, red and blue are swapped, touch does not respond, getTouch always returns false while pressing, touch coordinates are offset or mirrored — or when bringing up a new physical unit or board variant, calibrating touch, changing pins/SPI config in include/cyd_board.h or include/LGFX_CYD*.hpp, wiring the LVGL flush/indev callbacks, fixing upload/serial-enumeration failures, or explaining the LovyanGFX-vs-esp32_smartdisplay stack choice. Not for writing tests (see three-tier-testing) or editor false errors (see clangd-xtensa-setup).
---

# CYD Hardware Bring-up & Per-Unit Tuning

## Identify the board first

**Two boards are supported, and every knob below differs between them.** Verify which is on
the desk before changing any config. `include/cyd_board.h` picks one from the `CYD_BOARD_*`
build flag and includes its `LGFX_*.hpp` (an `LGFX : lgfx::LGFX_Device` subclass); nothing
under `lib/` may ever branch on this.

| | **ESP32-3248S035** (default) | ESP32-2432S028 v3 |
|---|---|---|
| Env | `esp32dev_cyd35` | `esp32dev_cyd` |
| Panel | 3.5" **ST7796S**, 320×480 portrait | 2.8" **ST7789**, 240×320 run landscape |
| Config | `include/LGFX_CYD3248S035.hpp` | `include/LGFX_CYD2432S028.hpp` |
| Bus | **VSPI**, touch **shares** it | HSPI, touch on its own soft-SPI |
| Readable (SDO) | **no** — no `dev-shot`, use `make sim-shot SIM_PANEL=35` | yes |
| LDR | **not fitted** | GPIO34 |

The original single-Micro-USB board is **ILI9341** and is *not* supported. Both supported
boards are WROOM-32/32E, 4 MB, **no PSRAM**.

**Do not trust the ID registers to tell you which panel it is.** On the 3.5" board they read
back all zeros — SDO is unwired, so there is nothing to read. Identify it by what it
*renders*: a correct 320×480 portrait image is the proof. And drive TOUCH_CS high before any
panel probe on a shared bus, or the XPT2046 answers and you read its noise as panel data
(this cost real time: a probe returned `7FDF00` from a floating touch CS).

Upload/monitor: **use the Micro-USB port** — the USB-C port lacks CC resistors (documented
board flaw) and won't enumerate on many hosts. `pio device monitor` (115200 baud) needs an
interactive TTY; for scripted capture, read `/dev/ttyUSB0` with pyserial instead.

## Symptom → knob table

The firmware's startup color self-test (`run_display_test()` in `src_cyd/main.cpp`) fills the
screen RED → GREEN → BLUE → WHITE; use it to diagnose. All knobs are in that board's
`include/LGFX_*.hpp`:

| Symptom | Fix |
|---|---|
| Screen stays blank/solid-white/garbled | `cfg.spi_mode` 0 → 3 (some units need mode 3; solid white = backlight on, panel uninitialized) |
| Colors photo-negative | flip `cfg.invert` — but read the XOR trap below first |
| RED renders blue / BLUE renders red | flip `cfg.rgb_order` |
| Touch mapping off near edges | re-run `make touch-calib` (below); `getTouch()` returns already-mapped screen coords |
| Touch Y axis mirrored | `y_min > y_max` is the **intentional** axis flip — do not "fix" it to be ascending |
| Touch x/y transposed or rotated vs display | touch `cfg.offset_rotation` — tune this, not the calibration min/max |
| `getTouch()` always false while pressing | `cfg.pin_int` must be `-1` — see below |
| Panel corrupts when touched (shared bus) | both panel and touch cfg need `bus_shared = true` |

### Two traps that make you fix the wrong thing

**`cfg.invert` is XORed, not absolute.** LovyanGFX does
`write_command((invert ^ _cfg.invert) ? CMD_INVON : CMD_INVOFF)`, and `LGFX_Device::init_impl`
calls `invertDisplay(getInvert())` = `setInvert(false)` — so **`cfg.invert = true` means init
leaves the panel INVERTED**, and `gfx.invertDisplay(x)` means "x XOR the flag", never
"invert = x". Label probe phases by the *panel state* you expect, never by the argument you
passed. Getting this backwards shipped an inverted panel that a LovyanGFX-direct probe called
correct while LVGL rendered a photo-negative: **cross-check through the real render path, not
just a probe.**

**LovyanGFX's `uint32_t` colour overload is RGB888, not RGB565.** `fillRect(..., 0xF800)`
("red" in 565) paints **green**; `0x07E0` ("green") paints **blue**. Write `0xFF0000u`. This
bug was sitting in `test_hardware.cpp` and bit a fresh probe the same day.

## The pin_int rule (hardware-diagnosed)

**`cfg.pin_int = -1` — never 36, on either board.** LovyanGFX's touch read bails out whenever
it polls the IRQ pin high, but both CYDs wire T_IRQ to GPIO36 — an input-only pin with no pull —
and there is no external pull-up, so polling it is unreliable and blocks *all* touch reads.
With `-1`, LovyanGFX reads touch pressure over SPI directly on every poll. Verified on
hardware; the symptom of getting this wrong is `getTouch` always returning false.

## Pin maps

Both share the bus pins (SCLK 14, MOSI 13, MISO 12, DC 2, panel CS 15) and differ everywhere
else. GPIO12 is the MTDI strapping pin (sets flash voltage at boot) on both — standard for
these boards, but brick-adjacent, so do not repurpose it.

| | **3.5" ESP32-3248S035** | **2.8" ESP32-2432S028 v3** |
|---|---|---|
| Panel | ST7796S, **VSPI_HOST**, 40 MHz | ST7789, **HSPI_HOST** |
| Panel RST | **4** | -1 |
| Backlight | **27** (PWM ch 7) | **21** (PWM ch 7) |
| Touch bus | **shares the display's VSPI** (`bus_shared = true` on both cfgs) | its own **soft SPI** (`spi_host = -1`, bit-banged): SCLK 25, MOSI 32, MISO 39 |
| Touch CS | 33 | 33 |
| Touch `offset_rotation` | 0 | 2 |
| Link UART (§2) | rx **25** / tx **32** | rx **22** / tx **27** |
| `cfg.invert` / `rgb_order` | false / false | false / false |
| `cfg.readable` | **false** (SDO unwired) | true |

The link pins move *because* the 3.5"'s touch shares the display bus: 25/32 are the 2.8"'s
bit-banged touch pins, and 27 is now the backlight. That is the whole story behind the change.

`offset_rotation` (both display and touch) is a **per-board calibration constant**, not a
relationship to maintain. `Panel_LCD::setRotation` composes the runtime rotation with the
panel's fixed offset, and `Panel_Device::convertRawXY` composes the *same* rotation with the
touch's — so touch tracks the display through all four rotations with no math on our side.
Set `PANEL_ROTATION` (0..3) in that board's `[board_*]` section; never patch a header.

## Touch calibration

`make touch-calib` (`CALIB_ENV=touch_calib_cyd` for the 2.8", `PORT=` to pick the board)
flashes `tools/touch_calibrate/`, which walks a **15-point grid**, least-squares-fits each
axis, and prints ready-to-paste `CALIB cfg.x_min = ...` lines plus residuals in pixels. Paste
them into that board's LGFX header.

Three things worth knowing before chasing a "bad" fit:

- `setCalibrate`'s convention is raw-at-screen-edge: `x_min` is the raw X read at screen x=0
  and `x_max` the raw X at x=w-1 — *not* the smallest/largest raw values.
- **Do not fit from `getTouch()` output.** Those coordinates are already mapped through the
  *current* calibration, so fitting them is circular and will produce confident nonsense. Tap
  known targets and fit the **raw** values, which is what the tool does.
- ~3 px mean residual on the 3.5" is the panel, not the fit: its raw-X slope differs left vs
  right (10.4 vs 11.5 raw/px) and drifts ~65 counts with Y — unrepresentable in an
  axis-aligned min/max model, and irrelevant against a 65 px touch target.

## LVGL glue rules (src_cyd/main.cpp)

- These are **PSRAM-less** WROOM-32 boards: partial draw buffers (RGB565), never full-frame.
  Sized in **scanlines** (`DRAW_BUF_LINES` in `cyd_board.h`), not as a screen fraction — 1/10
  of a 320×480 panel is *double* the 2.8"'s buffer, ×2 for double-buffering, on a board whose
  WiFi build already sits near the malloc cliff. Lines hold the DMA chunk and the DRAM cost
  constant across panels.
- Flush callback: compute width/height as `int32_t` (matches LVGL's `lv_area_t` and
  LovyanGFX's `setAddrWindow`/`pushPixels` signatures — avoids narrowing warnings).
- RGB565 byte swap must happen **exactly once**: `gfx.pushPixels((uint16_t *)px_map,
  w * h, true)` — the `true` does the swap. Do not also call `lv_draw_sw_rgb565_swap`;
  double-swapping produces wrong colors that look like an `rgb_order` problem.
- `loop()` must call `lv_tick_inc(elapsed_ms)` then `lv_timer_handler()` every iteration.
- In `include/lv_conf.h`: color depth is RGB565 (16-bit); `LV_USE_TFT_ESPI` and
  `LV_USE_LOVYAN_GFX` stay `0` — the flush/indev callbacks call LovyanGFX directly, not
  LVGL's built-in drivers. LVGL logging is on (`LV_USE_LOG` + `LV_LOG_PRINTF`) for serial
  debugging.

## Why LovyanGFX (stack rationale)

LovyanGFX has its own SPI panel/touch driver built on stable Arduino APIs, so it builds
against the latest arduino-esp32/ESP-IDF with no version pins and no board-definition
submodule; a generic `esp32dev_cyd` profile suffices. The alternative, `esp32_smartdisplay`,
wraps ESP-IDF's internal `esp_lcd` headers (which churn every IDF release), does not build
on current toolchains without pinning old versions plus header shims, and even then hits a
runtime DMA-alloc failure on this PSRAM-less board. No separate touch library is needed —
LovyanGFX handles both.

## Verify on hardware

`pio test -e embedded_cyd35` / `-e embedded` (board on Micro-USB) asserts `gfx.init()`, rotated
dimensions, brightness, ≥50 KB heap headroom, and a human-in-the-loop touch target.

To capture what's actually on the glass (`make dev-shot`) or inject touches
(`make dev-touch`) over WiFi while diagnosing, see the **ui-development** skill's
device tools (requires a uidev firmware via `make dev-flash`, which builds
`esp32dev_cyd35_uidev`; the 2.8" one is `esp32dev_cyd_uidev`).

## When NOT to use this skill

- Writing or debugging tests, or deciding where new code lives → **three-tier-testing**.
- Editor-only false errors in this file or `src_cyd/main.cpp` → **clangd-xtensa-setup**.
- CI lint failures / formatting → **lint-format-toolchain**.
