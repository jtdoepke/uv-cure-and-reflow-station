---
name: clangd-xtensa-setup
description: This skill should be used when VSCode/clangd shows false errors in the ESP32 firmware code (src_cyd/main.cpp, include/LGFX_CYD2USB.hpp) — "unknown argument '-mlongcalls'", "riscv/rv_utils.h file not found", "machine/endian.h file not found", "gnu/stubs-32.h" errors, pointer-size static_assert failures in ESP-IDF headers, or "unknown target 'xtensa'" — or when compile_commands.json is missing/stale, after changing build_flags in platformio.ini, or when running `make compiledb`. Not for real `pio run` build errors (compiler truth, not clangd) and not for `make tidy` findings (see lint-format-toolchain).
---

# clangd + ESP32 Xtensa Editor Setup

clangd is clang-based and cannot parse the ESP32 Xtensa **GCC** build out of the box. Three
committed mechanisms fix it against the generated `compile_commands.json`:

1. **`.clangd`** — strips Xtensa-only GCC flags, adds `--target=xtensa` + `-D__XTENSA__=1`,
   and `-nostdlibinc`.
2. **`tools/clangd-inject-sysincludes.py`** — bakes the Xtensa toolchain's builtin
   system-include dirs into every DB entry as `-isystem` (run automatically by
   `make compiledb`).
3. **`.vscode/settings.json`** — points clangd at the DB and disables Microsoft IntelliSense
   so the two linters don't double-report. (No `--query-driver` — see below.)

## Standard runbook

```bash
pio run            # once — the ESP32 toolchain must be installed for the inject script
make compiledb     # pio run -e esp32dev_cyd -t compiledb + tools/clangd-inject-sysincludes.py
```

Then reload the clangd server: Command Palette → "clangd: Restart language server".
Re-run `make compiledb` after changing `build_flags` in `platformio.ini`, adding source
files, or updating the espressif32 platform.

## Symptom → mechanism table

| Error in editor | Cause | Fix |
|---|---|---|
| `unknown argument '-mlongcalls'` (also `-mtext-section-literals`, `-fstrict-volatile-bitfields`, …) | clang rejects Xtensa-only GCC flags | `.clangd` `CompileFlags.Remove` list — add any new offending flag there |
| Pointer-size / `static_assert` errors in ESP-IDF headers; `riscv/rv_utils.h file not found` | clangd mis-detects target as 64-bit host and takes the RISC-V header branch | `--target=xtensa` + `-D__XTENSA__=1` in `.clangd` |
| `machine/endian.h file not found`, missing `stdio.h`-level headers | clang can't self-discover the GCC cross-toolchain's newlib headers | `make compiledb` — the inject script adds them as `-isystem`; toolchain must be installed (`pio run` once) |
| Fatal `gnu/stubs-32.h` error truncating LovyanGFX headers | host `/usr/include` (glibc) leaking in | `-nostdlibinc` in `.clangd` drops clang's host system dirs |
| `unknown target 'xtensa'` | the clangd build lacks the Xtensa target (needs clang ≥ 19 or Espressif's esp-clang) | drop the `--target=xtensa` line from `.clangd` (flag errors still clear) or install esp-clang |

## Why the inject script instead of --query-driver

clangd's `--query-driver` probes the GCC driver with the final flag set — but `.clangd`
adds clang-only flags (`--target=xtensa`) that `xtensa-esp32-elf-g++` rejects, so the probe
fails. The script instead asks the compiler named in the DB for its `#include <...>` search
dirs directly and writes them into the DB, making it self-contained. The DB is git-ignored,
so the absolute per-machine toolchain paths never get committed.

## Interaction with make tidy

`make tidy` temporarily regenerates a host (`native_ui_cyd`) compile DB, then restores the
esp32dev_cyd DB by re-running `make compiledb`. If editor diagnostics go weird after a tidy
run, re-run `make compiledb` and restart the clangd server.

Tidy's firmware pass consumes a sanitized copy of the esp32dev_cyd DB (`.pio/tidy-esp32/`)
produced by `tools/tidy-sanitize-compiledb.py`, which **mirrors this repo's `.clangd`
Remove/Add flag lists** (clang-tidy doesn't read `.clangd`). When adding or removing a
flag in `.clangd`, make the same change in that script.

## When NOT to use this skill

- An error also appears in a real `pio run` build → it's a genuine compile error, fix the code.
- `make tidy` / standalone clang-tidy findings → see the **lint-format-toolchain** skill
  (tidy lints `lib/**` via a host DB and `src_cyd/*` via a sanitized esp32dev_cyd DB).
- Test failures or native-build header leaks → see the **three-tier-testing** skill.
