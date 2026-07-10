---
name: lint-format-toolchain
description: This skill should be used when CI's lint job fails, when setting up formatting/linting on a fresh clone (mise install, pre-commit, make hooks), when clang-format or clang-tidy is not found or `make format` and the pre-commit hook disagree, when running or interpreting `make tidy`, when adding/modifying hooks in .pre-commit-config.yaml, or when bumping clang-format/clang-tidy versions. Not for editor squiggles in firmware code (see clangd-xtensa-setup) or test failures (see three-tier-testing).
---

# Linting, Formatting & Toolchain

CI's `lint` job (`.github/workflows/ci.yml`, `pre-commit/action`) runs exactly the hooks in
`.pre-commit-config.yaml` — local `make lint` and CI cannot disagree. clang-tidy is **not**
in pre-commit or CI by design: it is local/advisory only.

## Fresh-clone setup

```bash
mise install                        # fetches uv, then clang-format/clang-tidy PyPI wheels
mise reshim                         # only if a tool isn't found afterwards (shims mode)
pip install pre-commit && make hooks   # installs the git pre-commit hook (one-time)
```

`mise install` is self-contained — it needs only `mise` (no system pipx/python/conda).
Activate mise so its shims are on `PATH`; the Makefile calls `clang-format`/`clang-tidy`
directly.

## Command map

| Command | What it does |
|---|---|
| `make format` | clang-format in place (tracked C/C++, minus `include/lv_conf.h`) |
| `make format-check` | dry-run with `--Werror` |
| `make lint` (alias `make check`) | `pre-commit run --all-files` — clang-format + whitespace/EOF + `yamllint` + `markdownlint` |
| `make tidy` | clang-tidy over `lib/**/*.cpp` (advisory, local only) |

## Version-sync rule (checkable)

If bumping `"pipx:clang-format"` in `mise.toml`, bump the `mirrors-clang-format` `rev` in
`.pre-commit-config.yaml` to the same version **in the same commit** — and vice versa.
Otherwise `make format` and the hook produce different output and CI fails on freshly
formatted code. clang-tidy's patch version need **not** match clang-format's (independent
tools; the tidy wheel ships fewer patch releases).

## The lv_conf.h exclusion set

`include/lv_conf.h` is kept line-for-line comparable to LVGL's upstream template so LVGL
upgrades stay diffable. **Never reformat it.** The exclusion is enforced in four places
that must stay consistent:

1. `.pre-commit-config.yaml` — top-level `exclude` regex (authoritative; also excludes
   `get-platformio.py` and `compile_commands.json`)
2. `.clang-format-ignore` — covers editor-on-save and direct `clang-format` runs
3. `.editorconfig` — `[include/lv_conf.h]` section disables whitespace/EOF trimming
4. `Makefile` — `CXX_FILES` uses the `':!:include/lv_conf.h'` pathspec

If adding a new format/whitespace hook, add the `lv_conf.h` exclusion to it. If a diff
shows `include/lv_conf.h` changes that weren't an intentional upstream sync, revert them.

## make tidy mechanics

clang-tidy can't target the ESP32 Xtensa toolchain, so `make tidy` lints only the
**host-buildable** library logic (`lib/**/*.cpp`): it regenerates a `native_ui` host
compile DB into `.pio/tidy/`, runs `clang-tidy -p .pio/tidy`, then restores the esp32dev
DB at root (for clangd) via `make compiledb`. Requirements and known patterns:

- The ESP32 toolchain must be installed (`pio run` once) or the final `compiledb` step fails.
- The firmware glue (`src/main.cpp`, `include/LGFX_CYD2USB.hpp`) is linted by clangd in
  the editor instead — see the **clangd-xtensa-setup** skill.
- Known fix pattern: narrowing-conversion warnings in LVGL flush code → use `int32_t` for
  area width/height (matches LVGL's `lv_area_t` coords and LovyanGFX's
  `setAddrWindow`/`pushPixels` signatures), and `nullptr` over `NULL`.
- Config: `.clang-tidy` (Arduino/ESP32/LVGL headers filtered out via `HeaderFilterRegex`).

## Pre-commit checklist

Before committing: `make lint` clean, `make test` green, `make build` compiles, and
`include/lv_conf.h` absent from the diff (unless intentionally syncing with upstream LVGL).

## When NOT to use this skill

- False errors only in the editor (`-mlongcalls`, missing headers) → **clangd-xtensa-setup**.
- `pio test` failures or native-build header leaks → **three-tier-testing**.
- Display/touch misbehavior on the board → **hardware-bringup**.
