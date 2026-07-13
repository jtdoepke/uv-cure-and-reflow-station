# UI fonts

`red_hat_mono_14.c` is a generated LVGL bitmap font — the project's default UI font
(`LV_FONT_DEFAULT` in `include/lv_conf.h`). It is committed (not built in CI) so the firmware
and host builds need no font toolchain.

`red_hat_mono_28.c` is a second, larger size used **per-widget** (not the global default) for
big numeric readouts + glove-sized keys — the value-stepper editor's value + `−/+` glyphs (§24)
and the on-screen keypad's digits + `⌫`/`✓`/`✕` keys (§26). It is declared in `theme.h`
(`LV_FONT_DECLARE(red_hat_mono_28)`) and applied via `lv_obj_set_style_text_font`. It carries
ASCII `0x20–0x7F` + `°` (`0xB0`) plus the three Font Awesome glyphs the keypad keys use:
`0xF00C` (`LV_SYMBOL_OK` ✓), `0xF00D` (`LV_SYMBOL_CLOSE` ✗), `0xF55A` (`LV_SYMBOL_BACKSPACE` ⌫).

## What's in it

A single self-contained font merging two sources so no LVGL fallback font is needed:

- **Red Hat Mono** (SemiBold, weight 600) — ASCII `0x20–0x7F` + the degree sign `0xB0`
  (for `°C`). A squared, technical **monospace** typeface chosen for small-size legibility and
  digit disambiguation on the 320×240 HMI: it renders `l 1 I` distinctly and a **slashed zero**
  (`0`) by default, and its fixed advance keeps the stepper/keypad readouts column-aligned.
  License: SIL Open Font License 1.1.
- **Font Awesome 6 Free (Solid)** — only the glyphs LVGL's `LV_SYMBOL_*` map to and that the UI
  uses: `0xF053` (`LV_SYMBOL_LEFT` ‹ — the Settings ‹ Back button), `0xF00C` (`LV_SYMBOL_OK` ✓),
  `0xF00D` (`LV_SYMBOL_CLOSE` ✗), `0xF071` (`LV_SYMBOL_WARNING` ⚠), and `0xF077`/`0xF078`
  (`LV_SYMBOL_UP`/`LV_SYMBOL_DOWN` chevrons — the selectable-list ▲/▼ footer, §23/§24). License:
  SIL Open Font License 1.1.

Both licenses permit embedding the font in distributed firmware.

## Regenerating

Requires `npx` (Node) and `uvx` (uv) — both already provided by mise (see `mise.toml`), plus
the source TTFs (not committed — fetch fresh). Google Fonts ships Red Hat Mono only as a
**variable** font, and `lv_font_conv` cannot select a named instance from a variable font, so
first instance it to the SemiBold weight (`wght=600`) with `fonttools`:

```sh
# Red Hat Mono variable font -> static SemiBold (wght=600)
curl -sSL -o 'RedHatMono[wght].ttf' \
  'https://github.com/google/fonts/raw/main/ofl/redhatmono/RedHatMono%5Bwght%5D.ttf'
uvx --from fonttools fonttools varLib.instancer \
  'RedHatMono[wght].ttf' wght=600 -o RedHatMono-SemiBold.ttf

# Font Awesome 6 Free Solid
curl -sSL -o fa-solid-900.ttf \
  https://github.com/FortAwesome/Font-Awesome/raw/6.x/webfonts/fa-solid-900.ttf

npx -y lv_font_conv \
  --font RedHatMono-SemiBold.ttf -r '0x20-0x7F,0xB0' \
  --font fa-solid-900.ttf -r '0xF053,0xF00C,0xF00D,0xF071,0xF077,0xF078' \
  --size 14 --bpp 4 --format lvgl --no-compress \
  -o red_hat_mono_14.c
```

The larger `red_hat_mono_28.c` is the same Red Hat Mono SemiBold source at a bigger size, plus
the three Font Awesome glyphs the keypad keys need (`⌫`/`✓`/`✕`, §26):

```sh
npx -y lv_font_conv \
  --font RedHatMono-SemiBold.ttf -r '0x20-0x7F,0xB0' \
  --font fa-solid-900.ttf -r '0xF00C,0xF00D,0xF55A' \
  --size 28 --bpp 4 --format lvgl --no-compress \
  -o red_hat_mono_28.c
```

After regenerating (either size), re-apply one edit: `lv_font_conv` emits an
`#ifdef LV_LVGL_H_INCLUDE_SIMPLE … "lvgl/lvgl.h"` include shim that doesn't resolve in this
project; replace it with `#include <lvgl.h>` (as the rest of the codebase includes LVGL).

The slashed zero is Red Hat Mono's **default** `0` glyph — `lv_font_conv` bakes in default
glyphs only, so no OpenType feature flag is involved. If a future source font hides its slashed
zero behind a `zero`/`ss01` alternate, it won't survive this pipeline; verify the rendered `0`
after any font swap (e.g. `make sim-shot`).

To add glyphs later (larger readout size, more symbols), extend the ranges and/or add a second
output size, then declare it via `LV_FONT_DECLARE` and apply per-widget with
`lv_obj_set_style_text_font`.
