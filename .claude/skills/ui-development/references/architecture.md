# LVGL UI Architecture (MVVM via Observer/Subject)

How to structure UI code in this repo as it grows past the demo screen. The backbone is
LVGL v9's built-in Observer/Subject API (`lv_subject_t` / `lv_observer_t`): put state and
logic in plain C++ "view models" that own subjects, keep view code (widget creation +
`bind_*` calls) thin, and let the two communicate **only through subjects**. LVGL's own
`lv_example_observer_2.c` comments its subject as "the only interface between the UI and
the application" — that is the pattern to copy. It is also v9's replacement for v8's
removed `lv_msg` pub/sub.

This is the single highest-leverage structural decision: it makes screen logic testable
on the host (`native_ui_cyd`) and decouples screens from each other.

## Layer map (doc pattern → this repo)

| Layer | Rule | Lives in |
|---|---|---|
| Model / app domain | plain C++, **no `lv_` calls** | `lib/app_logic/` |
| ViewModel | owns `lv_subject_t` members; exposes intent methods (`onStartPressed()`) that mutate subjects | `lib/ui_logic/` (e.g. `*_viewmodel.h/.cpp`) |
| View / screen | builds the widget tree, calls `lv_*_bind_*`; **no business logic** | `lib/ui_logic/` (e.g. `*_screen.cpp`) |
| HAL / adapter | display+touch drivers, LVGL glue | `src_cyd/main.cpp`, `include/cyd_board.h` + `include/LGFX_CYD*.hpp` (device); `sim/` (host) |

Only the first two layers carry logic, and both compile for the native test envs — the
same host-testability rule three-tier-testing already enforces.

## Subjects

```c
lv_subject_init_int(&temp_subject, 28);
lv_slider_bind_value(slider, &temp_subject);          // UI → subject
lv_label_bind_text(label, &temp_subject, "%d °C");    // subject → UI
lv_subject_add_observer(&temp_subject, app_cb, NULL); // subject → app logic
```

- Subject types: int, float, string, pointer, color, and **group**
  (`lv_subject_init_group`) for state derived from several values.
- Widget bindings: `lv_slider_bind_value`, `lv_label_bind_text`,
  `lv_obj_bind_flag_if_eq`, `lv_obj_bind_state_if_eq`, `lv_obj_bind_style`.
- Prefer `lv_subject_add_observer_obj()` when the observer's lifetime is tied to a
  widget — it auto-unsubscribes when the widget is deleted (no dangling observers).
- **Subjects are the only globals.** Declare them `extern` in one `subjects.h` and
  define them once (LVGL's docs model exactly this). Everything else stays local to its
  view model or view.

## Routing LVGL C callbacks to C++ members

You cannot pass a non-static member function as `lv_event_cb_t`. Two proven patterns:

```cpp
// Captureless lambda thunk (decays to a function pointer; zero cost):
lv_obj_add_event_cb(obj, [](lv_event_t *e) {
  static_cast<MyScreen *>(lv_event_get_user_data(e))->onClick(e);
}, LV_EVENT_CLICKED, this);

// Reusable templated trampoline (community pattern, LVGL issue #6438):
template <typename C, void (C::*M)(lv_event_t *)>
struct EventHandler {
  static void thunk(lv_event_t *e) { (static_cast<C *>(lv_event_get_user_data(e))->*M)(e); }
};
lv_obj_add_event_cb(btn, EventHandler<MyScreen, &MyScreen::onClick>::thunk,
                    LV_EVENT_CLICKED, this);
```

- Inside handlers prefer `lv_event_get_target_obj(e)` (typed, no cast) over
  `lv_event_get_target(e)` in C++.
- **Caveat:** the trampoline consumes the callback's `user_data` slot for `this`. If a
  per-widget payload is also needed, bundle it in a struct with the `this` pointer — or
  better, route the value through a subject instead.
- From the handler, call a view-model intent method; the view model mutates subjects;
  bindings update the widgets. Views never poke other views.

## Screens and navigation

- A small screen manager (stack with push/pop/replace) is the standard pattern. Two
  memory strategies: **create-all-up-front** (fast switching, more RAM) vs
  **create-on-demand** (`lv_obj_delete()` on leave; less RAM, small switch latency).
  On this PSRAM-less board, default to **create-on-demand** and only pin a screen in
  memory if switch latency is actually felt.
- **Home is the one pinned (cached) screen** — the realized exception to create-on-demand.
  It is the always-returned-to hub and its rebuild was measured at ~89 ms on the 3.5"
  (widget creation + the flex-layout recompute the first render pays after `lv_obj_clean`);
  keeping it resident makes return-to-home a screen swap (`lv_screen_load`), 203 → 114 ms,
  for ~9–14 kB of the 64 kB LVGL pool. See `src_cyd/main.cpp` (`g_home_scr`) and
  `perf/baseline/device-35.md`.
  - **A screen is safe to cache only if a resident instance re-shown renders pixels
    identical to a fresh rebuild.** Two tests: (1) every runtime-changing value is
    subject-bound (observers fire off-screen, so nothing goes stale — no imperative
    one-shot sets of live data); (2) it holds no view-state a rebuild would reset (scroll,
    selection/focus, sub-page, in-progress input, mid-flight animation). Home passes both;
    Settings and the value editors fail (2). A failing screen becomes cacheable only behind
    a **reset-on-show** step that forces that view-state back to the rebuild default.
- Pass state between screens **through subjects, not constructor args** — a recreated
  screen re-reads current state for free, and hub-and-spoke navigation (design guide)
  stays stateless.
- Reusable composites: prefer a factory function/class that assembles stock widgets (a
  labeled stepper, a status header). Subclassing `lv_obj_class` is only warranted for a
  true theme-integrated widget library — heavier than anything this project needs.

## Styles and theming

- **Design tokens first:** palette/spacing/radius as `constexpr` in one
  `lib/ui_logic/theme` header. One variable change restyles the whole UI. Screens call its
  `apply_*`/`add_*` helpers and never write a colour themselves.
- **This project styles inline, per widget — not through shared `lv_style_t` globals**, and
  that is deliberate: properties land in each widget's own style list and are freed with it,
  whereas a process-lifetime `lv_style_t` is stranded by the native tests' repeated
  `lv_init()`/`lv_deinit()` cycles, which reset LVGL's allocator underneath it. See the
  `theme.h` file header.
- If a screen with many repeated widgets ever makes the per-widget copies cost real RAM, a
  shared `static lv_style_t` + `lv_obj_add_style` is the escape hatch — but it must then
  **outlive every widget referencing it** (LVGL stores only the pointer, so a local is a
  use-after-free), and `LV_STYLE_CONST_INIT` keeps never-changing ones in flash.
- Styles, timers, and animations are *not* owned by widgets — keep them as
  members/statics or they leak. (`lv_style_transition_dsc_t` is a plain POD rather than an
  `lv_style_t`, so a `static` one is safe across `lv_deinit` — that is how `theme.cpp` owns
  the press/release feedback timing.)

## Custom draw callbacks (and the clip-area rule)

Line-art that no widget provides — corner brackets, the background dot matrix — is drawn from a
`LV_EVENT_DRAW_MAIN_END` callback rather than from child objects. Two reasons: a callback sees
the object's **live state** for free (a child does not inherit its parent's state, so child
brackets could not hide themselves on a disabled button), and it lands on the object's outer
bounds without having to undo the caller's padding.

**A draw callback MUST clip its work to `layer->_clip_area`.** On this board that is
correctness, not optimisation:

- With no PSRAM the display renders in **partial scanline chunks** (`DRAW_BUF_LINES`, 24 lines —
  a fixed line count, deliberately *not* a fraction of the screen; see `cyd_board.h` and
  CLAUDE.md). A callback on the **screen** therefore fires **once per chunk** — ~13 times for a
  320×480 redraw, ~10 for a 320×240 one.
- The dot matrix originally queued every dot on the panel on every one of those calls: ~7,200
  draw tasks per screen instead of ~600. A Home → Settings transition took **~3 s** on real
  glass. It compounded with the translucent tiles, whose every press forces the canvas beneath
  them to recomposite and re-run the whole loop.
- **The simulator will not warn you** — it renders identical pixels on a host CPU. This class of
  bug is only visible on the device, which is what the device loop is for.

Bound the loop to the intersection of the object's coords and the clip area, and — for a
repeating pattern — start it on the **lattice**, not the clip edge, or the pattern shifts
depending on which chunk is redrawing:

```cpp
lv_area_t clip;
clip.x1 = LV_MAX(coords.x1, layer->_clip_area.x1);   // intersect by hand: lv_area_intersect()
clip.y1 = LV_MAX(coords.y1, layer->_clip_area.y1);   // lives in the private lv_area_private.h
clip.x2 = LV_MIN(coords.x2, layer->_clip_area.x2);
clip.y2 = LV_MIN(coords.y2, layer->_clip_area.y2);
if (clip.x1 > clip.x2 || clip.y1 > clip.y2) return;
const int32_t x0 = coords.x1 + ((clip.x1 - coords.x1) / STEP) * STEP;  // snap to the lattice
```

Draw-order note: `DRAW_MAIN_END` puts your work on top of the object's own background but under
its children — which is why the dot matrix sits on the canvas yet is occluded by every opaque
panel, exactly as a background should be.

- Runtime theme switching (e.g. a dark/high-brightness mode): bind alternate styles with
  `lv_obj_bind_style(obj, &style_dark, sel, &dark_subject, 1)` — flipping one subject
  reskins everything, no per-widget code.

## Threading (the dominant runtime constraint)

**LVGL is not thread-safe** — no `lv_*` call (including `lv_timer_handler()`) may run
concurrently with another. In this firmware, Arduino `loop()` *is* the single UI task,
and everything currently runs there, so it's safe by construction. Keep it that way:

- Future FreeRTOS tasks (heater PID, sensor sampling, WiFi) must **never call `lv_*`**.
  Use the **gateway pattern**: push data to the UI task (queue or volatile snapshot) and
  translate to subject writes inside `loop()`. `src_cyd/ui_dev_tools.cpp` already does this —
  the web server arms volatile touch state that the LVGL read callback consumes.
- Only if a shared-mutex design ever becomes unavoidable: set `LV_USE_OS` and wrap calls
  in `lv_lock()`/`lv_unlock()`. Prefer the gateway; it has no lock ordering to get wrong.
- If tasks are added, pin the UI to one core and keep WiFi/BLE and heavy work on the
  other (`xTaskCreatePinnedToCore`, generous stack — LVGL rendering is stack-hungry).

## Memory and C++ feature costs on this board

No PSRAM (WROOM-32), so common LVGL advice to put canvases/screens in PSRAM does **not** apply here:

- Draw buffers: partial (`DRAW_BUF_LINES` = 24 scanlines) in internal DRAM, period. A
  `region dram0_0_seg overflowed` link error means static data grew too big — the fix
  here is heap allocation at `setup()` (as `src_cyd/main.cpp` does for the draw buffer since
  WiFi joined the uidev env), not "move to PSRAM".
- Normal OO C++ (classes, RAII, virtual, templates) is fine in UI code; vtables live in
  flash and UI never runs in an ISR. Keep exceptions and RTTI out of the codebase —
  use return codes; LVGL and this firmware are not exception-safe.
- `std::function` heap-allocates for capturing lambdas — fine for a handful of app-level
  callbacks, avoid in per-frame/per-flush paths. A captureless lambda is free.
- Skip generic RAII widget wrappers (lvglpp et al.): LVGL deletes children with their
  parent, so naive owning wrappers double-free, and correct ones cost ownership-flag
  bookkeeping for little gain. The house pattern — factory function returning a struct
  of `lv_obj_t *` handles (see `main_ui.h`) — is deliberate.

## Deliberately not used here (don't "helpfully" migrate)

- **`LV_CONF_SKIP` / config-via-`-D`-flags:** this repo keeps a real `include/lv_conf.h`
  wired by `LV_CONF_PATH`, line-comparable to upstream, with `#ifndef` guards for
  per-env toggles. Do not switch approaches.
- **`esp_lvgl_port` / esp-bsp:** ESP-IDF components; this project is Arduino + LovyanGFX
  with hand-written flush/indev glue (see hardware-bringup for why).
- **SquareLine / EEZ Studio / LVGL XML:** UI is hand-written. If a generator is ever
  adopted, its output is the **view layer only** — callbacks point at view-model
  methods, widgets bind to subjects, and nothing edits the generated folder.
- **Version churn caveat:** v8→v9 removed `lv_msg` and overhauled display/indev APIs,
  and v9 minor bumps have caused UI freezes in the field. `platformio.ini` allows minor
  updates (`lvgl@^9.5.0`); after any LVGL bump, run `make test` and eyeball
  `make sim-shot` before trusting the device build.
