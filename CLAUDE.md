# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

Firmware for an ESP32 "Cheap Yellow Display" (CYD) — specifically the **dual-USB
ESP32-2432S028 v3** variant, which has a **ST7789** SPI TFT (320x240, inverted colors /
BGR relative to the older ILI9341 boards) and an XPT2046 resistive touch controller. Built
with PlatformIO + Arduino framework. The display and touch are driven by **LovyanGFX**, the
UI by **LVGL 9.5**. `src/main.cpp` runs a startup color self-test, then shows a "Hello CYD!"
label and a touch-counting button. Verified working end-to-end on hardware.

> The board is the "7789" v3 board (both Micro-USB and USB-C ports). NOT the original
> single-Micro-USB ILI9341 board, nor the USB-C-only v2.

## Stack rationale (why LovyanGFX)

LovyanGFX has its own SPI panel/touch driver built on stable Arduino APIs, so it builds
against the **latest** arduino-esp32 / ESP-IDF with **no version pins and no board-definition
submodule**. The obvious alternative, `esp32_smartdisplay`, wraps ESP-IDF's internal `esp_lcd`
headers, which churn every IDF release — on the current toolchain it does not build without
pinning to old/unstable versions plus header shims, and even then hits a runtime DMA-alloc
failure on this PSRAM-less board. LovyanGFX was chosen to avoid that lock-in. All
display/touch config lives in `include/LGFX_CYD2USB.hpp`; a generic `esp32dev` board profile
is sufficient (WROOM-32, 4 MB flash, no PSRAM).

## Commands

PlatformIO is the build system. If `pio` is not on PATH, use `~/.platformio/penv/bin/pio` or
`python get-platformio.py` to install it. Single env: `esp32dev`. **Always run `pio` from the
project root** (a stray `cd` into `.pio/libdeps/...` makes it read a library's own
`platformio.ini`).

- Build: `pio run`
- Upload + monitor: `pio run -t upload -t monitor` — **use the Micro-USB port**; the USB-C
  port lacks CC resistors (a documented board flaw) and won't enumerate on many hosts.
- Serial monitor: `pio device monitor` (115200 baud). Note: `pio device monitor` needs an
  interactive TTY; for scripted capture, read `/dev/ttyUSB0` with pyserial instead.
- Clean (needed if an `lv_conf.h` edit seems ignored): `pio run -t clean`
- Unit tests: `pio test` (the `test/` dir is currently an empty scaffold)

## Architecture & key gotchas

- **Single translation unit.** All logic lives in `src/main.cpp` (`setup()` / `loop()`).
  `loop()` must call `lv_tick_inc(elapsed_ms)` then `lv_timer_handler()` every iteration.
  Display init is `gfx.init()`; the LVGL flush and touch-read callbacks call the `LGFX`
  object (`gfx.pushPixels` / `gfx.getTouch`) directly.

- **Display config is `include/LGFX_CYD2USB.hpp`** (an `LGFX` subclass): ST7789 panel on
  HSPI/SPI2 (SCLK14/MOSI13/MISO12/DC2/CS15/RST-1, BL21), XPT2046 touch on software SPI
  (SCLK25/MOSI32/MISO39/CS33). Per-unit knobs: if the screen is blank, change `cfg.spi_mode`
  0→3; if colors are photo-negative, flip `cfg.invert`; if red/blue swap, flip `cfg.rgb_order`.

- **Touch requires `cfg.pin_int = -1` — do not set it to 36.** LovyanGFX's touch read bails
  out whenever it polls the IRQ pin high, but the CYD wires T_IRQ to GPIO36 (an input-only
  pin with no internal pull) and there's no external pull-up, so polling it is unreliable and
  blocks *all* touch reads (symptom: `getTouch` always returns false even while pressing).
  With `-1`, LovyanGFX reads touch pressure over SPI directly on every poll, which works.
  This was diagnosed on hardware — see `include/LGFX_CYD2USB.hpp`.

- **Touch calibration** is the `x_min/x_max/y_min/y_max` in the touch config; `getTouch()`
  returns already-mapped screen coordinates. The current values work; refine per unit if the
  mapping feels off near the edges.

- **LVGL config** is `include/lv_conf.h`, wired in via `LV_CONF_PATH`. That flag must use the
  `'"..."'` nesting so the value reaches the compiler as a quoted string literal (else LVGL
  fails with `#include expects "FILENAME"`). Color depth is RGB565 (16-bit); `LV_USE_TFT_ESPI`
  and `LV_USE_LOVYAN_GFX` are both `0` (we call LovyanGFX directly, not via LVGL's built-in
  drivers). LVGL logging is on (`LV_USE_LOG` + `LV_LOG_PRINTF`).

- **This is a PSRAM-less ESP32-WROOM board.** Use partial LVGL draw buffers (~1/10 screen);
  never a full-frame buffer.

## Dependencies (managed by PlatformIO)

Declared in `platformio.ini` `lib_deps`, fetched into `.pio/libdeps/` (git-ignored, do not
edit): `lovyan03/LovyanGFX` and `lvgl` 9.5 (pinned to match `include/lv_conf.h`). No board
submodule, no touch library — LovyanGFX handles both display and touch.

## Notes

- `.pio/`, `compile_commands.json`, `get-platformio.py`, and generated `.vscode/*` files are
  build/tooling artifacts (all git-ignored). Don't hand-edit them.
- Recommended editor tooling is the PlatformIO IDE extension (see `.vscode/extensions.json`).
