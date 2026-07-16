# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

Firmware for a two-MCU oven/UV-cure station (see `docs/design.md`):

- **CYD (HMI)** — an ESP32 "Cheap Yellow Display" with XPT2046 resistive touch, driven by
  **LovyanGFX** with the UI in **LVGL 9.5**. **Two boards are supported**, chosen by env:
  `esp32dev_cyd35` (the **default**: ESP32-3248S035, 3.5" **ST7796S**, 320x480 **portrait**)
  and `esp32dev_cyd` (ESP32-2432S028, 2.8" ST7789, 240x320 run landscape). Both verified
  end-to-end on hardware. The 3.5" is the default because it survives WiFi bring-up and the
  2.8" browns out (§21) — without a radio there is no OTA, the controller's only field
  reflash path (§25). `src_cyd/main.cpp` is the composition root and holds no pin literal:
  the board's pins, capabilities and orientation live in `include/cyd_board.h`, which
  dispatches to `include/LGFX_CYD3248S035.hpp` / `include/LGFX_CYD2432S028.hpp`. LVGL config
  in `include/lv_conf.h`; panel geometry in `lib/panel/panel.h`.
- **Controller** — a second ESP32 (WROOM-32E) that will own the oven's safety-critical
  outputs. `src_control/main.cpp` (env `esp32dev_control`) is thin glue over ports in
  `lib/control_port/` + logic in `lib/control_logic/`.
- They share `lib/protocol`: nanopb-generated messages from `proto/oven.proto` (codegen
  runs in the build; a pre-script bakes a schema hash into both firmwares — see §9 of the
  design doc). CYD-specific dirs/envs carry a `_cyd` postfix, controller ones `_control`.

Built with PlatformIO + Arduino.

> The 2.8" board is the "7789" v3 board (both Micro-USB and USB-C ports). NOT the original
> single-Micro-USB ILI9341 board, nor the USB-C-only v2.

## Guardrails (always apply)

- **Always run `pio` from the project root** — a stray `cd` into `.pio/libdeps/...` makes
  it read a library's own `platformio.ini`.
- **Never reformat `include/lv_conf.h`** — it's kept line-for-line comparable to LVGL's
  upstream template. It's excluded from every lint/format hook; keep it that way.
- **Testable logic and UI belong in `lib/`, not `src_cyd/main.cpp`.** Anything that
  `#include`s `LovyanGFX.hpp` cannot compile for the native test target, so only the
  firmware adapter (`src_cyd/main.cpp`, `include/cyd_board.h` + the `LGFX_*.hpp` headers) may
  touch LGFX.
- **Nothing under `lib/` may branch on a board identity.** `CYD_BOARD_*` is read by
  `include/cyd_board.h` and nowhere else. `lib/` sees only neutral facts: geometry via
  `PANEL_*` (`lib/panel/panel.h` — use `panel::kPortrait`, never a board name) and
  capabilities as **data** (`subj_has_ambient_light`, `device_info.h`). The native envs
  enforce this at build level — they have no board at all.
- **Never edit anything under `.pio/`** or the generated artifacts
  (`compile_commands.json`, `get-platformio.py`, `.vscode/c_cpp_properties.json`).
- **Touch requires `cfg.pin_int = -1` — do not set it to 36** on either board
  (hardware-diagnosed; see the hardware-bringup skill for why).
- **`make dev-shot` does not work on the 3.5" board** — its panel's SDO is unwired, so GRAM
  readback returns zeros. The endpoint returns 501 rather than serving a black PNG; use
  `make sim-shot SIM_PANEL=35`.
- **Upload over the Micro-USB port** — the USB-C port lacks CC resistors and won't
  enumerate on many hosts.
- **Identify a board by its ESP32 MAC, never by `ttyUSBn`** — with multiple devkits attached
  the `ttyUSB0`/`ttyUSB1` numbering is assigned in random plug order, and **both CYDs use the
  same CH340 bridge** (`1a86:7523`) so USB VID:PID / `/dev/serial/by-id` can't tell them apart
  (only the controller's CP2102 `10c4:ea60` is distinguishable that way). `esptool.py -p
  <port> read_mac` is read-only — it reads the eFuse MAC and reboots into the existing
  firmware, never writing flash — so run it before any upload. Known MACs (unit-specific;
  update this line if a board is swapped): `8c:94:df:92:21:e4` = **2.8" CYD** (`esp32dev_cyd`),
  `b0:cb:d8:03:5c:d8` = **3.5" CYD** (`esp32dev_cyd35`, default), `c0:cd:d6:cb:e7:e4` =
  **controller** (`esp32dev_control`). Map every port at once, then pass the confirmed device
  as `PORT=`:

  ```sh
  for d in /dev/ttyUSB*; do mac=$(esptool.py -p "$d" read_mac 2>/dev/null | awk '/^MAC:/{print $2; exit}'); echo "$d $mac"; done
  # 8c:94:df:92:21:e4=CYD2.8  b0:cb:d8:03:5c:d8=CYD3.5  c0:cd:d6:cb:e7:e4=controller
  ```

- **No PSRAM on either board**: partial LVGL draw buffers, never full-frame. Sized in
  **scanlines** (`DRAW_BUF_LINES`, `cyd_board.h`), not as a fraction of the screen — a
  fraction would double the DRAM cost on the bigger panel, which is the wrong invariant.
- The `LV_CONF_PATH` build flag must keep its `'"..."'` quote nesting so the value reaches
  the compiler as a quoted string literal.
- `src_cyd/main.cpp` is thin glue: it owns the `LGFX` object and the LVGL flush/touch
  callbacks; `loop()` must call `lv_tick_inc(elapsed_ms)` then `lv_timer_handler()` every
  iteration.

## Commands

PlatformIO is the build system; `pio` and `protoc` are expected on PATH (mise provides
them — see `mise.toml`; run `mise install` on a fresh clone). The default CYD firmware env is
`esp32dev_cyd35` (the 3.5" board, what `pio run` builds); `esp32dev_cyd` is the 2.8" one, and
the controller's is `esp32dev_control`. Every env comes in board pairs — `<env>` for the 2.8",
`<env>35` for the 3.5" (`esp32dev_cyd35_uidev`, `embedded_cyd35`, `native_ui_cyd_35`,
`native_sim_35`, `touch_calib_cyd35`). The
`esp32dev_control_bench` / `esp32dev_cyd_bench` pair is the two-devkit bench (backlog A8):
it moves the **controller's** link to UART2 so UART0 stays free for USB — on a devkit the
USB bridge's TX fights the CYD's on the controller's RX0, so production's link-on-UART0
(§2/§25) can't coexist with both boards plugged in. The `Makefile` wraps common
invocations (`make help` lists them).

- Build: `make build` (every firmware + both boards; a variant that isn't built rots) or
  `pio run` (the default CYD only)
- Upload + monitor: `pio run -t upload -t monitor` (115200 baud)
- Host tests: `make test` — runs the UI suites at **both** geometries (`native_ui_cyd` +
  `native_ui_cyd_35`), which is what keeps the portrait/landscape branches honest
- See the UI / simulate clicks: `make sim-shot ARGS="click 160 120"` → Read
  `.pio/sim/ui.png`; add `SIM_PANEL=35` for the 3.5" portrait panel (details →
  ui-development skill)
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
