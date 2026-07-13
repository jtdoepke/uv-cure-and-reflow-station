# UI fonts

`jetbrains_mono_14.c` is a generated LVGL bitmap font — the project's default UI font
(`LV_FONT_DEFAULT` in `include/lv_conf.h`). It is committed (not built in CI) so the firmware
and host builds need no font toolchain.

## What's in it

A single self-contained font merging two sources so no LVGL fallback font is needed:

- **JetBrains Mono** (Regular) — ASCII `0x20–0x7F` + the degree sign `0xB0` (for `°C`). A
  monospace typeface chosen for small-size legibility and digit disambiguation on the 320×240
  HMI. License: SIL Open Font License 1.1.
- **Font Awesome 6 Free (Solid)** — only the three glyphs LVGL's `LV_SYMBOL_*` map to and that
  the UI uses: `0xF00C` (`LV_SYMBOL_OK` ✓), `0xF00D` (`LV_SYMBOL_CLOSE` ✗), `0xF071`
  (`LV_SYMBOL_WARNING` ⚠). License: SIL Open Font License 1.1.

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
  --font fa-solid-900.ttf -r '0xF00C,0xF00D,0xF071' \
  --size 14 --bpp 4 --format lvgl --no-compress \
  -o jetbrains_mono_14.c
```

After regenerating, re-apply one edit: `lv_font_conv` emits an
`#ifdef LV_LVGL_H_INCLUDE_SIMPLE … "lvgl/lvgl.h"` include shim that doesn't resolve in this
project; replace it with `#include <lvgl.h>` (as the rest of the codebase includes LVGL).

To add glyphs later (larger readout size, more symbols), extend the ranges and/or add a second
output size, then declare it via `LV_FONT_DECLARE` and apply per-widget with
`lv_obj_set_style_text_font`.
