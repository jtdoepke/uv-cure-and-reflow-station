# LVGL 9.5 Test API Notes (native_ui env)

Working reference for `test/test_ui/` suites. Everything here is gated by `LV_USE_TEST=1`
(set by the `native_ui` env; `#ifndef`-guarded in `include/lv_conf.h`).

## Setup / teardown pattern

```cpp
#include <unity.h>
#include <lvgl.h>
#include "src/debugging/test/lv_test.h" // lv_test_* helpers (gated by LV_USE_TEST)

void setUp(void) {
  lv_init();
  lv_test_display_create(320, 240);  // in-memory dummy display, no pixels on glass
  lv_test_indev_create_all();        // simulated mouse/keypad/encoder
}
void tearDown(void) {
  lv_deinit();                       // fresh LVGL per test; static widget state must not leak
}
```

The `lv_test.h` include path is relative to the lvgl library root — copy it verbatim from
`test/test_ui/test_main_ui.cpp`.

## Driving the UI

```cpp
lv_obj_update_layout(lv_screen_active()); // force layout before reading coords
lv_test_mouse_click_at(cx, cy);           // also: lv_test_mouse_press() / _release()
lv_test_wait(50);                         // advances ticks + runs lv_timer_handler each ms
TEST_ASSERT_EQUAL_STRING("Touched 1", lv_label_get_text(ui.btn_label));
```

Widget coordinates come from `lv_obj_get_coords()` after `lv_obj_update_layout()` — do not
hardcode pixel positions.

## Memory-leak checks

`lv_test_get_free_mem()` before/after a create-delete loop. LVGL's docs recommend exactly
**32 bytes** of tolerance for fragmentation:
`if (LV_ABS((int64_t)mem2 - (int64_t)mem1) > 32) fail;`
Requires `LV_USE_STDLIB_MALLOC = LV_STDLIB_BUILTIN` (the `lv_conf.h` default here). For
whole-app stress runs, LVGL's own threshold is "< 100 bytes permanent leak is normal".

## Screenshot regression (not enabled)

`lv_test_screenshot_compare("ref.png")` needs `LV_USE_TEST_SCREENSHOT_COMPARE=1` +
`LV_USE_LODEPNG` + 32-bit color and allocates a few MB with standard malloc — a
desktop-only technique, currently off in `include/lv_conf.h`. Enable only if pixel-level
regressions become a real problem. For *visual inspection* (rendering the UI to a PNG to
look at, not regression-compare), use the `native_sim` simulator instead — see the
**ui-development** skill.

## Observer/Subject pattern (for future screens)

For oven-controller screens, model widget state as `lv_subject_t` values
(`lv_subject_init_int` / `lv_subject_set_int`) and bind widgets with `lv_label_bind_text`,
`lv_slider_bind_value`, `lv_obj_bind_flag_if_eq`. Screen-transition logic then tests as
plain subject reads/writes — no clicking simulation needed for state-machine coverage.

## SDL escalation path (deliberately not set up)

LovyanGFX ships an SDL desktop backend (`Panel_sdl`, `#define LGFX_SDL`) that could render
real pixels on the host and exercise the actual flush path. It needs SDL2, a display server
(or `xvfb-run`/`SDL_VIDEODRIVER=dummy` in CI), and a harness around its blocking
main-thread event loop. **Escalation trigger:** only build such an env if UI regressions
(color/layout bugs visible on glass) repeatedly slip past the dummy-display tests. Note
the cheaper escalations that already exist in the **ui-development** skill: the
`native_sim` PNG simulator (host, headless) and the on-device screenshot/touch API.
