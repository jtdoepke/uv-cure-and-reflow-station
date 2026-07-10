---
name: hardware-bringup
description: This skill should be used when the CYD display or touch hardware misbehaves — screen stays blank/white/garbled, colors look photo-negative, red and blue are swapped, touch does not respond, getTouch always returns false while pressing, touch coordinates are offset or mirrored — or when bringing up a new physical unit, changing pins/SPI config in include/LGFX_CYD2USB.hpp, wiring the LVGL flush/indev callbacks, fixing upload/serial-enumeration failures, or explaining the LovyanGFX-vs-esp32_smartdisplay stack choice. Not for writing tests (see three-tier-testing) or editor false errors (see clangd-xtensa-setup).
---

# CYD Hardware Bring-up & Per-Unit Tuning

## Identify the board first

This project targets the **dual-USB ESP32-2432S028 v3** (both Micro-USB **and** USB-C
ports) = **ST7789** panel, `cfg.offset_rotation = 0`, `cfg.invert = false`. The original
single-Micro-USB board is **ILI9341** with different settings — verify which board is on
the desk before changing any config, or every knob below will mislead. All display/touch
config lives in `include/LGFX_CYD2USB.hpp` (an `LGFX : lgfx::LGFX_Device` subclass).

Upload/monitor: **use the Micro-USB port** — the USB-C port lacks CC resistors (documented
board flaw) and won't enumerate on many hosts. `pio device monitor` (115200 baud) needs an
interactive TTY; for scripted capture, read `/dev/ttyUSB0` with pyserial instead.

## Symptom → knob table

The firmware's startup color self-test (`run_display_test()` in `src/main.cpp`) fills the
screen RED → GREEN → BLUE → WHITE; use it to diagnose. All knobs are in
`include/LGFX_CYD2USB.hpp`:

| Symptom | Fix |
|---|---|
| Screen stays blank/garbled | `cfg.spi_mode` 0 → 3 (some units need mode 3) |
| Colors photo-negative | flip `cfg.invert` |
| RED renders blue / BLUE renders red | flip `cfg.rgb_order` |
| Touch mapping off near edges | tune touch `x_min/x_max/y_min/y_max`; `getTouch()` returns already-mapped screen coords |
| Touch Y axis mirrored | `y_min > y_max` (3700 > 200) is the **intentional** axis flip — do not "fix" it to be ascending |
| `getTouch()` always false while pressing | `cfg.pin_int` must be `-1` — see below |

## The pin_int rule (hardware-diagnosed)

**`cfg.pin_int = -1` — never 36.** LovyanGFX's touch read bails out whenever it polls the
IRQ pin high, but the CYD wires T_IRQ to GPIO36 — an input-only pin with no internal pull —
and there is no external pull-up, so polling it is unreliable and blocks *all* touch reads.
With `-1`, LovyanGFX reads touch pressure over SPI directly on every poll. Verified on
hardware; the symptom of getting this wrong is `getTouch` always returning false.

## Pin map (dual-USB v3)

- **Display** — ST7789 on HSPI/SPI2 (`HSPI_HOST`): SCLK 14, MOSI 13, MISO 12, DC 2, CS 15,
  RST -1; backlight PWM on GPIO 21 (`Light_PWM`, channel 7).
- **Touch** — XPT2046 on its own **software SPI** bus (`spi_host = -1`, bit-banged, so it
  coexists with the display bus): SCLK 25, MOSI 32, MISO 39, CS 33.

## LVGL glue rules (src/main.cpp)

- This is a **PSRAM-less** WROOM-32 board: use a partial draw buffer (~1/10 screen,
  RGB565), never a full-frame buffer.
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
submodule; a generic `esp32dev` profile suffices. The alternative, `esp32_smartdisplay`,
wraps ESP-IDF's internal `esp_lcd` headers (which churn every IDF release), does not build
on current toolchains without pinning old versions plus header shims, and even then hits a
runtime DMA-alloc failure on this PSRAM-less board. No separate touch library is needed —
LovyanGFX handles both.

## Verify on hardware

`pio test -e embedded` (board on Micro-USB) asserts `gfx.init()`, rotated 320×240
dimensions, brightness, ≥50 KB heap headroom, and a human-in-the-loop touch target.

## When NOT to use this skill

- Writing or debugging tests, or deciding where new code lives → **three-tier-testing**.
- Editor-only false errors in this file or `src/main.cpp` → **clangd-xtensa-setup**.
- CI lint failures / formatting → **lint-format-toolchain**.
