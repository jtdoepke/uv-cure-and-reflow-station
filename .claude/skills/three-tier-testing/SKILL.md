---
name: three-tier-testing
description: This skill should be used when writing, running, or debugging tests (pio test -e native_logic / native_ui / embedded), when adding new business logic or UI code and deciding where it goes (lib/app_logic vs lib/ui_logic vs src/main.cpp), when a native test build fails on ESP-IDF/Arduino/LovyanGFX headers, when tests aren't discovered, when LVGL won't initialize in a test, or when isolating a single test case. Not for on-hardware display/touch misbehavior (see hardware-bringup) or CI lint failures (see lint-format-toolchain).
---

# Three-Tier Testing

## The architecture rule that makes testing possible

Anything that `#include`s `LovyanGFX.hpp` cannot compile for the native target (its
`Bus_SPI`/`Panel_*` layers need ESP-IDF/Arduino SPI, GPIO, DMA headers). Therefore:

- **New business logic** → `lib/app_logic/`, tested in `test/test_logic/`. Depends only on
  the `IDisplay`/`ITouch` ports (`lib/display_port/`); tests inject fakes from
  `test/helpers/fake_touch.h`. `lib/app_logic/tap_counter.h` is the pattern to copy.
- **New UI** → `lib/ui_logic/`, tested in `test/test_ui/` on LVGL's headless dummy display.
  `lib/ui_logic/main_ui.cpp` + `test/test_ui/test_main_ui.cpp` are the pattern to copy.
- **Only the firmware adapter** (`src/main.cpp`, `include/LGFX_CYD2USB.hpp`) touches LGFX.

If a native build errors on ESP-IDF/Arduino/SPI headers, a LovyanGFX include leaked past
the ports — find and remove it; do not add stubs. `lib_ignore` in the env is the reliable
exclusion mechanism (`build_src_filter` alone fails because the Library Dependency Finder
still scans `#include`s).

## Per-tier runbook

Run from the project root (a stray `cd` into `.pio/libdeps/...` makes `pio` read a
library's own `platformio.ini`).

| Command | What runs | Excluded |
|---|---|---|
| `pio test -e native_logic` | pure logic vs fakes, host GCC | `lib_ignore = LovyanGFX lvgl`; ArduinoFake available (`lib_compat_mode = off`, `gnu++17`) |
| `pio test -e native_ui` | LVGL 9.5 headless (`LV_USE_TEST=1`), real `lib/ui_logic` widgets, simulated input | `lib_ignore = LovyanGFX` |
| `pio test -e embedded` | on the real board via Micro-USB: `gfx.init()`, geometry, brightness, heap, human-in-the-loop touch | needs hardware; not run in CI |

`make test` = the two native envs; CI (`.github/workflows/ci.yml`) runs those plus a
`pio run -e esp32dev` compile-check on every push. Nothing under `.pio/` is ever edited —
all config is env-scoped in `platformio.ini`.

## PlatformIO/Unity mechanics (each one has bitten before)

- Only `test/` subfolders named `test_*` are collected; each builds as its own runner
  binary with its own `main()` (native) or `setup()/loop()` (embedded). Duplicated
  `main()`s across suites are expected, not an error.
- The **suite** is the unit of granularity (selected per env via `test_filter`). There is
  no run-one-case flag — to isolate a case, temporarily comment out the other
  `RUN_TEST(...)` lines in that suite's runner.
- Shared helpers and `unity_config.h` live at the `test/` root, which is on the include
  path for every suite (`#include "helpers/fake_touch.h"`).
- `setUp()`/`tearDown()` run before/after **each** test. In `test_ui`, `lv_init()` /
  `lv_deinit()` per test keeps static widget state from leaking between cases.
- On-target runners need `delay(2000)` before `UNITY_BEGIN()` — the board resets after
  upload and the host must reconnect to serial, or early output (sometimes the whole run)
  is lost. Keep on-target tests short to avoid watchdog reboots.
- `LV_USE_TEST` is `#ifndef`-guarded in `include/lv_conf.h` so `native_ui` flips it on via
  `-D LV_USE_TEST=1` without a redefinition clash; firmware/embedded leave it off.
- If an `include/lv_conf.h` edit seems ignored, run `pio run -t clean`.
- LVGL test API details (display/indev creation, click simulation, leak checks, SDL
  escalation path): see `references/lvgl-test-notes.md` in this skill.

## New-feature checklist ("what good looks like")

1. Logic lives behind a port in `lib/` — no `LovyanGFX.hpp` include outside the adapter.
2. A native test exists in the matching suite (`test_logic` or `test_ui`).
3. `make test` green, `make build` still compiles.
4. `make lint` clean (see **lint-format-toolchain**).

## When NOT to use this skill

- Display/touch wrong **on the physical board** (colors, blank screen, dead touch) →
  **hardware-bringup**; native tests can't see those failures.
- CI `lint` job failures, formatting, clang-tidy → **lint-format-toolchain**.
- Editor-only false errors in firmware glue → **clangd-xtensa-setup**.
