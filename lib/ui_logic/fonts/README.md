# UI fonts

`jetbrains_mono_14.c` is a generated LVGL bitmap font — the project's default UI font
(`LV_FONT_DEFAULT` in `include/lv_conf.h`). It is committed (not built in CI) so the firmware
and host builds need no font toolchain.

`jetbrains_mono_28.c` is a second, larger size used **per-widget** (not the global default) for
big numeric readouts + glove-sized keys — the value-stepper editor's value + `−/+` glyphs (§24)
and the on-screen keypad's digits + `⌫`/`✓`/`✕` keys (§26). It is declared in `theme.h`
(`LV_FONT_DECLARE(jetbrains_mono_28)`) and applied via `lv_obj_set_style_text_font`. It carries
ASCII `0x20–0x7F` + `°` (`0xB0`) plus the three Font Awesome glyphs the keypad keys use:
`0xF00C` (`LV_SYMBOL_OK` ✓), `0xF00D` (`LV_SYMBOL_CLOSE` ✗), `0xF55A` (`LV_SYMBOL_BACKSPACE` ⌫).

## What's in it

A single self-contained font merging two sources so no LVGL fallback font is needed:

- **JetBrains Mono** (Regular) — ASCII `0x20–0x7F` + the degree sign `0xB0` (for `°C`). A
  monospace typeface chosen for small-size legibility and digit disambiguation on the 320×240
  HMI. License: SIL Open Font License 1.1.
- **Font Awesome 6 Free (Solid)** — only the glyphs LVGL's `LV_SYMBOL_*` map to and that the UI
  uses: `0xF053` (`LV_SYMBOL_LEFT` ‹ — the Settings ‹ Back button), `0xF00C` (`LV_SYMBOL_OK` ✓),
  `0xF00D` (`LV_SYMBOL_CLOSE` ✗), `0xF071` (`LV_SYMBOL_WARNING` ⚠), and `0xF077`/`0xF078`
  (`LV_SYMBOL_UP`/`LV_SYMBOL_DOWN` chevrons — the selectable-list ▲/▼ footer, §23/§24). License:
  SIL Open Font License 1.1.

Both licenses permit embedding the font in distributed firmware.

## Regenerating

Requires `npx` (Node) and the two source TTFs (not committed — fetch fresh):

```sh
# JetBrains Mono
curl -sSL -o JetBrainsMono-Regular.ttf \
  https://github.com/JetBrains/JetBrainsMono/raw/master/fonts/ttf/JetBrainsMono-Regular.ttf
# Font Awesome 6 Free Solid
curl -sSL -o fa-solid-900.ttf \
  https://github.com/FortAwesome/Font-Awesome/raw/6.x/webfonts/fa-solid-900.ttf

npx -y lv_font_conv \
  --font JetBrainsMono-Regular.ttf -r '0x20-0x7F,0xB0' \
  --font fa-solid-900.ttf -r '0xF053,0xF00C,0xF00D,0xF071,0xF077,0xF078' \
  --size 14 --bpp 4 --format lvgl --no-compress \
  -o jetbrains_mono_14.c
```

The larger `jetbrains_mono_28.c` is the same JetBrains-Mono source at a bigger size, plus the
three Font Awesome glyphs the keypad keys need (`⌫`/`✓`/`✕`, §26):

```sh
npx -y lv_font_conv \
  --font JetBrainsMono-Regular.ttf -r '0x20-0x7F,0xB0' \
  --font fa-solid-900.ttf -r '0xF00C,0xF00D,0xF55A' \
  --size 28 --bpp 4 --format lvgl --no-compress \
  -o jetbrains_mono_28.c
```

After regenerating (either size), re-apply one edit: `lv_font_conv` emits an
`#ifdef LV_LVGL_H_INCLUDE_SIMPLE … "lvgl/lvgl.h"` include shim that doesn't resolve in this
project; replace it with `#include <lvgl.h>` (as the rest of the codebase includes LVGL).

To add glyphs later (larger readout size, more symbols), extend the ranges and/or add a second
output size, then declare it via `LV_FONT_DECLARE` and apply per-widget with
`lv_obj_set_style_text_font`.
