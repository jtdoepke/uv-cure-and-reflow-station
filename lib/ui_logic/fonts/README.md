# UI fonts

Generated LVGL bitmap fonts, committed (not built in CI) so the firmware and host builds need no
font toolchain. **Four files, two roles, two panel pitches:**

| role | 5.60 px/mm (2.8") | 6.49 px/mm (3.5") | selected by |
|---|---|---|---|
| default UI text | `red_hat_mono_14.c` | `red_hat_mono_16.c` | `LV_FONT_DEFAULT`, `include/lv_conf.h` |
| big readouts + keys | `red_hat_mono_28.c` | `red_hat_mono_32.c` | `theme::big_font()`, `theme.h` |

## Why two sizes of each

Glyphs are the one thing `theme.h`'s mm-authored tokens **cannot** scale: a font is a fixed grid
of pixels, so the same 14 px text is 2.50 mm on the 2.8" panel but only 2.16 mm on the denser
3.5" — a 14% physical shrink on the board that is becoming the default. 16 px restores it to
2.47 mm, and 32 px puts the big readout back to 4.93 mm (vs 28 px = 5.00 mm).

Both selections key on **`PANEL_PX_PER_MM_X100 >= 600`** — the pitch, never a board name, so
nothing under `lib/` learns a board identity and a future panel at 6.5 px/mm gets the right answer
with no edit. The threshold appears twice (`lv_conf.h` and `theme.h`) only because `lv_conf.h` is
included by LVGL's own sources and cannot pull in `panel.h`. Each build links **only** the two
fonts it uses — the others are never declared, so they cost no flash (`nm firmware.elf` confirms).

It is deliberately **not** a `-D LV_FONT_DEFAULT=&red_hat_mono_16` build flag: that value needs an
`&` and the custom-declare needs parens, `build_flags` are shell-parsed, and both land as a
*syntax error in the compile command* rather than as a missing define.

## Contents

`red_hat_mono_14/16` carry ASCII `0x20–0x7F` + `°` (`0xB0`) plus the Font Awesome glyphs the
chrome uses: `0xF053` (`LV_SYMBOL_LEFT` ‹), `0xF00C` (✓), `0xF00D` (✗), `0xF071` (⚠),
`0xF077`/`0xF078` (▲/▼). `red_hat_mono_28/32` are used **per-widget** (not the global default) for
big numeric readouts + glove-sized keys — the value-stepper's value + `−/+` glyphs (§24) and the
keypad's digits + `⌫`/`✓`/`✕` keys (§26), applied via `lv_obj_set_style_text_font`. They carry
ASCII + `°` plus only `0xF00C` (✓), `0xF00D` (✗), `0xF55A` (⌫).

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

`red_hat_mono_16.c` is the identical command with `--size 16 -o red_hat_mono_16.c`.

The larger `red_hat_mono_28.c` is the same Red Hat Mono SemiBold source at a bigger size, plus
the three Font Awesome glyphs the keypad keys need (`⌫`/`✓`/`✕`, §26):

```sh
npx -y lv_font_conv \
  --font RedHatMono-SemiBold.ttf -r '0x20-0x7F,0xB0' \
  --font fa-solid-900.ttf -r '0xF00C,0xF00D,0xF55A' \
  --size 28 --bpp 4 --format lvgl --no-compress \
  -o red_hat_mono_28.c
```

`red_hat_mono_32.c` is the identical command with `--size 32 -o red_hat_mono_32.c`.

**This recipe is verified reproducible**, and that is worth using rather than trusting: fetching
the sources fresh and re-running the 14 px command above regenerates the committed
`red_hat_mono_14.c` **byte-for-byte** (after the include-shim edit below). That is the check to
run before believing a newly generated size — if the 14 px output no longer matches what is
committed, upstream has moved and every size regenerated in that session would be from a
different font than the ones beside it.

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
