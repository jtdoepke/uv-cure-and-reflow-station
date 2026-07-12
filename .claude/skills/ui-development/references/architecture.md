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
| HAL / adapter | display+touch drivers, LVGL glue | `src_cyd/main.cpp`, `include/LGFX_CYD2USB.hpp` (device); `sim/` (host) |

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
- Pass state between screens **through subjects, not constructor args** — a recreated
  screen re-reads current state for free, and hub-and-spoke navigation (design guide)
  stays stateless.
- Reusable composites: prefer a factory function/class that assembles stock widgets (a
  labeled stepper, a status header). Subclassing `lv_obj_class` is only warranted for a
  true theme-integrated widget library — heavier than anything this project needs.

## Styles and theming

- **Design tokens first:** palette/spacing/radius as `constexpr` in one
  `lib/ui_logic/theme` header. One variable change restyles the whole UI.
- Shared `static lv_style_t` objects, initialized once, applied with
  `lv_obj_add_style`. Styles must **outlive every widget that references them** — LVGL
  stores only the pointer, so locals are a use-after-free. Use `LV_STYLE_CONST_INIT`
  for never-changing styles to keep them in flash (RAM matters on this board).
- Styles, timers, and animations are *not* owned by widgets — keep them as
  members/statics or they leak.
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

- Draw buffers: partial (~1/10 screen) in internal DRAM, period. A
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
