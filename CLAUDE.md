# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

Firmware for an ESP32 "Cheap Yellow Display" (CYD): ST7789 SPI TFT (320x240) + XPT2046
resistive touch, driven by **LovyanGFX** with the UI in **LVGL 9.5**. Built with
PlatformIO + Arduino. `src/main.cpp` runs a startup color self-test, then shows a demo
touch-counting UI. Verified working end-to-end on hardware. Display/touch config lives in
`include/LGFX_CYD2USB.hpp`; LVGL config in `include/lv_conf.h`.

> The board is the "7789" v3 board (both Micro-USB and USB-C ports). NOT the original
> single-Micro-USB ILI9341 board, nor the USB-C-only v2.

## Guardrails (always apply)

- **Always run `pio` from the project root** — a stray `cd` into `.pio/libdeps/...` makes
  it read a library's own `platformio.ini`.
- **Never reformat `include/lv_conf.h`** — it's kept line-for-line comparable to LVGL's
  upstream template. It's excluded from every lint/format hook; keep it that way.
- **Testable logic and UI belong in `lib/`, not `src/main.cpp`.** Anything that
  `#include`s `LovyanGFX.hpp` cannot compile for the native test target, so only the
  firmware adapter (`src/main.cpp`, `include/LGFX_CYD2USB.hpp`) may touch LGFX.
- **Never edit anything under `.pio/`** or the generated artifacts
  (`compile_commands.json`, `get-platformio.py`, `.vscode/c_cpp_properties.json`).
- **Touch requires `cfg.pin_int = -1` — do not set it to 36** (hardware-diagnosed; see the
  hardware-bringup skill for why).
- **Upload over the Micro-USB port** — the USB-C port lacks CC resistors and won't
  enumerate on many hosts.
- **No PSRAM on this board**: partial LVGL draw buffers (~1/10 screen), never full-frame.
- The `LV_CONF_PATH` build flag must keep its `'"..."'` quote nesting so the value reaches
  the compiler as a quoted string literal.
- `src/main.cpp` is thin glue: it owns the `LGFX` object and the LVGL flush/touch
  callbacks; `loop()` must call `lv_tick_inc(elapsed_ms)` then `lv_timer_handler()` every
  iteration.

## Commands

PlatformIO is the build system; `pio` is expected on PATH (mise provides it — see
`mise.toml`; run `mise install` on a fresh clone). The firmware env is `esp32dev` (the
`pio run` default). The `Makefile` wraps common invocations (`make help` lists them).

- Build: `pio run` (or `make build`)
- Upload + monitor: `pio run -t upload -t monitor` (115200 baud)
- Host tests: `make test` == `pio test -e native_logic -e native_ui`
- See the UI / simulate clicks: `make sim-shot ARGS="click 160 120"` → Read
  `.pio/sim/ui.png` (details → ui-development skill)
- Screenshot/touch the physical board over WiFi: `make dev-flash`, then
  `make dev-shot IP=<ip>` / `make dev-touch IP=<ip> X=160 Y=120` (→ ui-development skill)
- Clean (if an `lv_conf.h` edit seems ignored): `pio run -t clean`
- Lint: `make lint` before committing — CI (`.github/workflows/ci.yml`) runs the same
  hooks plus the native tests and a firmware compile-check.

## Skills

Project skills in `.claude/skills/` hold the detailed runbooks — load the matching one
instead of re-deriving:

- Blank/garbled screen, wrong colors, dead or offset touch, per-unit tuning, pin map,
  flush-callback rules, why-LovyanGFX → **hardware-bringup**
- Writing/running/debugging tests, where new code goes, native builds failing on
  ESP-IDF/Arduino headers, Unity/PlatformIO mechanics → **three-tier-testing**
- Editor/clangd false errors (`-mlongcalls`, `riscv/rv_utils.h`, `machine/endian.h`),
  `make compiledb` → **clangd-xtensa-setup**
- CI lint failures, fresh-clone toolchain setup (mise/pre-commit), `make tidy`,
  clang-format version bumps → **lint-format-toolchain**
- Designing screens (touch targets, colors, safety confirmations), seeing/iterating on
  the rendered UI, simulator screenshots, on-device screenshot + touch injection →
  **ui-development**

## Pointers

- Dependencies are declared in `platformio.ini` `lib_deps` (LovyanGFX + LVGL 9.5, fetched
  into git-ignored `.pio/libdeps/`). No board submodule, no touch library.
- Product domain (oven/UV-curing controller), donor hardware, and safety constraints live
  in `README.md`. Any future heater/UV control must layer software cutoffs on top of the
  hardware thermal fuse, never replace it.
