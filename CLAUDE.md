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
`python get-platformio.py` to install it. The firmware env is `esp32dev` (the `pio run`
default); there are also three test envs (see **Testing** below). **Always run `pio` from the
project root** (a stray `cd` into `.pio/libdeps/...` makes it read a library's own
`platformio.ini`). The `Makefile` wraps the common `pio`/lint invocations as convenience
targets (`make build`, `make test`, `make lint`, …); run `make help` to list them.

- Build: `pio run`
- Upload + monitor: `pio run -t upload -t monitor` — **use the Micro-USB port**; the USB-C
  port lacks CC resistors (a documented board flaw) and won't enumerate on many hosts.
- Serial monitor: `pio device monitor` (115200 baud). Note: `pio device monitor` needs an
  interactive TTY; for scripted capture, read `/dev/ttyUSB0` with pyserial instead.
- Clean (needed if an `lv_conf.h` edit seems ignored): `pio run -t clean`

## Linting & formatting

Run before committing — CI's `lint` job (`.github/workflows/ci.yml`) runs the same hooks and
fails the build on violations.

- **Format C++**: `make format` (clang-format in place) or `make format-check` (dry-run). Style
  is `.clang-format` (LLVM base, 2-space, 100-col, right-aligned pointers, attached braces).
- **All hooks**: `make lint` (== `pre-commit run --all-files`) — clang-format + whitespace/EOF,
  `yamllint`, `markdownlint`. Config in `.pre-commit-config.yaml`, `.yamllint.yaml`,
  `.markdownlint.yaml`. One-time: `pip install pre-commit && make hooks` (installs the git hook).
- **Never reformat `include/lv_conf.h`** — it's kept line-for-line comparable to LVGL's upstream
  template so LVGL upgrades stay diffable. It's excluded from *every* hook (top-level `exclude`
  in `.pre-commit-config.yaml`, plus `.clang-format-ignore` and `.editorconfig`).
- **Toolchain via mise**: `mise.toml` pins `clang-format` and `clang-tidy` (prebuilt PyPI
  wheels). Run `mise install` once, with mise activated so its shims are on `PATH` — the
  Makefile calls `clang-format`/`clang-tidy` directly. mise install is self-contained (only
  needs `mise`; it fetches `uv`, which installs the wheels — no system pipx/python). If a tool
  isn't found after `mise install` (shims mode), run `mise reshim`. The
  **clang-format version in `mise.toml` must stay in sync with the mirror `rev` in
  `.pre-commit-config.yaml`** so `make format` and the hook produce identical output.
- **clang-tidy** (`make tidy`) is **local/advisory only** — never in pre-commit or CI. It lints
  only the **host-buildable library logic** (`lib/**/*.cpp`) using a `native_ui` host compile DB
  it regenerates each run into `.pio/tidy/` (then restores the esp32dev DB at root for clangd).
  The firmware glue (`src/main.cpp`, `include/LGFX_CYD2USB.hpp`) needs the ESP32 Xtensa toolchain
  that clang-tidy can't target — that code is linted by **clangd in the editor** instead (below).
  Config: `.clang-tidy` (Arduino/ESP32/LVGL headers filtered out).

## Editor linting (clangd + ESP32 Xtensa)

VSCode's clangd is clang-based and can't parse the ESP32 Xtensa **GCC** build out of the box:
it rejects Xtensa-only flags (`-mlongcalls` …), mis-detects the target (pointer-size /
`static_assert` errors, and a wrong RISC-V header branch → `riscv/rv_utils.h not found`), and
can't find the toolchain's system headers (`machine/endian.h not found`). Two committed files fix
this against the generated `compile_commands.json` (run **`make compiledb`** to (re)generate it
for esp32dev):

- **`.clangd`** — strips the Xtensa-only flags, adds `--target=xtensa` (clang ≥ 19 / esp-clang
  has the Xtensa target, giving correct 32-bit sizes) and `-D__XTENSA__=1` (routes ESP-IDF
  headers down the Xtensa branch). If clangd reports `unknown target 'xtensa'`, its build lacks
  Xtensa support — drop that line (flag errors still clear) or use Espressif's **esp-clang**.
- **`.vscode/settings.json`** — points clangd at the DB and sets `--query-driver` so it asks
  `xtensa-esp32-elf-g++` for its builtin include paths (resolves the system headers); also
  disables Microsoft IntelliSense so the two linters don't double-report.

`make compiledb` also runs `tools/clangd-inject-sysincludes.py`, which bakes the Xtensa
toolchain's own system-include paths into the DB (clang can't self-discover the GCC
cross-toolchain's headers, and clangd's `--query-driver` is unreliable here). It queries the
compiler named in the DB, so **the ESP32 toolchain must be installed** (run `pio run` once) for
`make compiledb`/`make tidy` to work.

After editing these or regenerating the DB, reload the clangd server (Command Palette →
"clangd: Restart language server").

## Testing

A three-tier pyramid (strategy: `~/Downloads/cyd_lovyangfx_testing_strategy.md`). Nothing
under `.pio/` is ever edited; all config is env-scoped in `platformio.ini`.

- `pio test -e native_logic` — fast host tests of pure C++ logic (`lib/app_logic`) behind
  the `IDisplay`/`ITouch` ports (`lib/display_port`). LovyanGFX **and** LVGL are excluded
  via `lib_ignore`; tests inject fakes from `test/helpers/fake_touch.h`.
- `pio test -e native_ui` — LVGL 9.5 on the host with `LV_USE_TEST=1`, driving the real UI
  (`lib/ui_logic`) through LVGL's in-memory dummy display + simulated input. No board.
- `pio test -e embedded` — on the real board (needs the Micro-USB port). Asserts
  `gfx.init()`, rotated 320×240 dimensions, brightness, heap headroom, and a
  human-in-the-loop touch target.

**Architecture rule that makes this work:** testable code lives in `lib/` (compiled by both
firmware and tests). Anything that `#include`s `LovyanGFX.hpp` cannot compile for the native
target, so it stays behind the ports — only the firmware/production adapter touches LGFX.
Put new business logic in `lib/app_logic` (test in `test_logic`), new UI in `lib/ui_logic`
(test in `test_ui`). The native envs are the fast feedback loop; GitHub Actions
(`.github/workflows/ci.yml`) runs both plus a firmware compile-check on every push.

Gotchas: PlatformIO only collects `test/` subfolders named `test_*`, each built as its own
runner. The unit of granularity is the **suite** (one `test_*` folder, selected by env via
`test_filter`) — Unity/PlatformIO has no run-one-test-case flag, so to isolate a single case
temporarily comment out the other `RUN_TEST(...)` lines in that suite's `main()`. `LV_USE_TEST`
is `#ifndef`-guarded in `lv_conf.h` so `native_ui` flips it on with `-D LV_USE_TEST=1` while
firmware keeps it off. On-target tests need `delay(2000)` before `UNITY_BEGIN()` (serial
reconnect after the post-upload reset).

## Architecture & key gotchas

- **`src/main.cpp` is thin glue** (`setup()` / `loop()`): it owns the `LGFX` object and the
  LVGL flush/touch callbacks (which call `gfx.pushPixels` / `gfx.getTouch` directly), runs
  the color self-test, then hands off to `create_main_ui()`. `loop()` must call
  `lv_tick_inc(elapsed_ms)` then `lv_timer_handler()` every iteration. **Testable logic and
  UI belong in `lib/`, not `main.cpp`** — see the architecture rule under **Testing**. UI
  construction already lives in `lib/ui_logic/main_ui.cpp`.

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
