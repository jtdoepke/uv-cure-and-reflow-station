# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

Firmware for a two-MCU oven/UV-cure station (see `docs/design.md`):

- **CYD (HMI)** — ESP32 "Cheap Yellow Display": ST7789 SPI TFT (320x240) + XPT2046
  resistive touch, driven by **LovyanGFX** with the UI in **LVGL 9.5**. `src_cyd/main.cpp`
  runs a startup color self-test, then shows a demo touch-counting UI. Verified working
  end-to-end on hardware. Display/touch config lives in `include/LGFX_CYD2USB.hpp`; LVGL
  config in `include/lv_conf.h`.
- **Controller** — a second ESP32 (WROOM-32E) that will own the oven's safety-critical
  outputs. `src_control/main.cpp` (env `esp32dev_control`) is thin glue over ports in
  `lib/control_port/` + logic in `lib/control_logic/`.
- They share `lib/protocol`: nanopb-generated messages from `proto/oven.proto` (codegen
  runs in the build; a pre-script bakes a schema hash into both firmwares — see §9 of the
  design doc). CYD-specific dirs/envs carry a `_cyd` postfix, controller ones `_control`.

Built with PlatformIO + Arduino.

> The board is the "7789" v3 board (both Micro-USB and USB-C ports). NOT the original
> single-Micro-USB ILI9341 board, nor the USB-C-only v2.

## Guardrails (always apply)

- **Always run `pio` from the project root** — a stray `cd` into `.pio/libdeps/...` makes
  it read a library's own `platformio.ini`.
- **Never reformat `include/lv_conf.h`** — it's kept line-for-line comparable to LVGL's
  upstream template. It's excluded from every lint/format hook; keep it that way.
- **Testable logic and UI belong in `lib/`, not `src_cyd/main.cpp`.** Anything that
  `#include`s `LovyanGFX.hpp` cannot compile for the native test target, so only the
  firmware adapter (`src_cyd/main.cpp`, `include/LGFX_CYD2USB.hpp`) may touch LGFX.
- **Never edit anything under `.pio/`** or the generated artifacts
  (`compile_commands.json`, `get-platformio.py`, `.vscode/c_cpp_properties.json`).
- **Touch requires `cfg.pin_int = -1` — do not set it to 36** (hardware-diagnosed; see the
  hardware-bringup skill for why).
- **Upload over the Micro-USB port** — the USB-C port lacks CC resistors and won't
  enumerate on many hosts.
- **No PSRAM on this board**: partial LVGL draw buffers (~1/10 screen), never full-frame.
- The `LV_CONF_PATH` build flag must keep its `'"..."'` quote nesting so the value reaches
  the compiler as a quoted string literal.
- `src_cyd/main.cpp` is thin glue: it owns the `LGFX` object and the LVGL flush/touch
  callbacks; `loop()` must call `lv_tick_inc(elapsed_ms)` then `lv_timer_handler()` every
  iteration.

## Commands

PlatformIO is the build system; `pio` and `protoc` are expected on PATH (mise provides
them — see `mise.toml`; run `mise install` on a fresh clone). The CYD firmware env is
`esp32dev_cyd` (the `pio run` default); the controller's is `esp32dev_control`. The
`esp32dev_control_bench` / `esp32dev_cyd_bench` pair is the two-devkit bench (backlog A8):
it moves the **controller's** link to UART2 so UART0 stays free for USB — on a devkit the
USB bridge's TX fights the CYD's on the controller's RX0, so production's link-on-UART0
(§2/§25) can't coexist with both boards plugged in. The `Makefile` wraps common
invocations (`make help` lists them).

- Build: `make build` (both firmwares) or `pio run` (CYD only)
- Upload + monitor: `pio run -t upload -t monitor` (115200 baud)
- Host tests: `make test` == `pio test -e native_logic_cyd -e native_ui_cyd -e native_control`
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
  the rendered UI, simulator screenshots, on-device screenshot + touch injection,
  structuring UI code (view models, `lv_subject_t` bindings, styles/themes, LVGL
  callbacks in C++) → **ui-development**

## Pointers

- Dependencies are declared in `platformio.ini` `lib_deps` (LovyanGFX + LVGL 9.5 + nanopb,
  fetched into git-ignored `.pio/libdeps/`). No board submodule, no touch library.
- The wire contract is `proto/oven.proto` (+ `proto/oven.options` nanopb bounds). Generated
  `.pb.c/.pb.h` and `schema_hash.h` land under `.pio/build/<env>/` — never committed, never
  edited. `Hello` and its frame-type id are a frozen append-only contract; everything else
  may churn because the schema hash gates the link (design.md §9).
- Product domain (oven/UV-curing controller), donor hardware, and safety constraints live
  in `README.md`. Any future heater/UV control must layer software cutoffs on top of the
  hardware thermal fuse, never replace it.
