---
name: ui-development
description: This skill should be used when designing or iterating on UI screens for the oven controller — creating a new screen, choosing layout/colors/touch-target sizes, adding controls for heat or UV, "take a screenshot of the UI", "what does the screen look like", "click the button and show me", verifying layout/color/spacing changes after editing lib/ui_logic, changing the palette/accent or adding alert/alarm/fault styling, "the tiles look washed out", "why is the disabled button brighter than the enabled one", capturing screenshots / injecting touches on the physical board over WiFi, or structuring UI code — view models, lv_subject_t observer bindings, screen navigation, styles/themes, wiring LVGL event callbacks to C++. Not for writing UI unit tests (see three-tier-testing) or colour inversion / red-blue swap / dead touch on glass (see hardware-bringup).
---

# UI Development: Iterate Visually, Design for Gloves

Three concerns: **the loop** (how to see and drive the UI while editing it), **the design
rules** (what every screen must satisfy — this is a glove-operated hazardous machine, not
a phone app), and **the code architecture** (how screens are structured so they stay
host-testable). Check new/changed screens against the design rules before calling them
done.

## The host loop (default — seconds per iteration, no hardware)

1. Edit UI code in `lib/ui_logic/` (never `src_cyd/main.cpp`; see three-tier-testing).
2. `make sim-shot ARGS="<actions>"` — builds the `native_sim` env, renders the real
   `lib/ui_logic` widgets headlessly, runs the scripted actions, writes a PNG.
   **Add `SIM_PANEL=35` for the default 3.5″ 320×480 portrait panel** (`native_sim_35`).
   The two geometries are different layouts (tokens scale, flows flip), so a screenshot is
   only evidence about the panel it was rendered for — check both when changing layout.
3. Read `.pio/sim/ui.png` (the Read tool renders PNG) and inspect.
4. Repeat.

Action grammar (positional tokens, executed in order; a final screenshot is always
written to `--out`, default `.pio/sim/ui.png`):

| Action | Effect |
|---|---|
| `click X Y` | full press+release at (X, Y) |
| `press X Y` | press and hold at (X, Y) — for drags: press, moveto…, release |
| `moveto X Y` | move the (pressed or idle) pointer |
| `release` | release the pointer |
| `wait MS` | advance LVGL time by MS ms (timers + animations run) |
| `shot PATH` | write an intermediate PNG at this point in the sequence |
| `frame PATH` | capture **without settling** — for photographing motion (→ design-guide.md) |
| `temp N` | set the chamber reading |
| `state idle\|hot\|running\|fault` | drive the machine-state badge |
| `link ok\|none\|schema` | drive the controller link — **`link none` is the only way to see the disabled/bracket-less tile treatment** |
| `sensor on\|off` | ambient-light sensor fitted; `off` = the 3.5″ (read when a panel is *built*, so put it before the clicks that open one) |

`--screen` picks what to render: `home` (default), `settings`, `list`, `stepper`, `keypad`,
and `alerts` — a **style specimen** showing the whole caution/alarm/fault vocabulary
(`apply_alert` / `apply_pill` / `apply_fault_panel` / `alarm_pulse`) on one canvas. `alerts`
is the only view where the accent and all three reserved state hues are live at once, which
is what makes it the right place to judge a palette change; Home only ever shows one state.

Examples:

- Just look: `make sim-shot`
- Press the demo button and look: `make sim-shot ARGS="click 160 120"`
- Before/after pair: `make sim-shot ARGS="shot .pio/sim/before.png click 160 120"`
- Custom output: `make sim-shot SIM_OUT=.pio/sim/settings.png ARGS="click 40 210"`
- Another screen: `make sim-shot SIM_PANEL=35 ARGS="--screen alerts"` — `ARGS` is appended
  after `--out`, so flags pass through and you still get a rebuild. Prefer this to running
  `.pio/build/*/program` by hand, which will happily render a stale binary.
- A disabled/no-link Home: `make sim-shot SIM_PANEL=35 ARGS="link none"`

Coordinates are the same space the matching `native_ui_cyd` / `native_ui_cyd_35` tests use
(`lv_test_mouse_click_at`) — 320×240 landscape by default, 320×480 portrait under
`SIM_PANEL=35`. The sim renders RGB565 exactly as the device does (same `lv_conf.h`, same
color depth), so colors/dithering match the firmware rasterizer.

**The screenshot settles before it captures**, and that matters: LVGL's default theme animates
style changes over 80 ms, and the mode tiles change state as soon as the link subject moves
(subjects boot at `LINK_NONE`). An unsettled shot catches tiles mid-blend and reads exactly
like a washed-out palette bug that does not exist. `write_png()` waits for
`lv_anim_count_running() == 0`; do not remove that.

**To photograph motion, use `frame`, not `shot`** — an infinite animation (the §22 alarm pulse)
never settles, so `shot` captures an arbitrary phase of it. Recipe for sampling a full period:
`references/design-guide.md`. Everything else keeps `shot`.

The harness lives in `sim/sim_main.cpp` (a host CLI, not firmware, not a test suite).
Exit codes: 0 ok, 1 usage error, 2 PNG write failure; prints `WROTE <path>` per capture.

## The device loop (escalation — real pixels on glass)

Escalate only for what the dummy renderer cannot show: LovyanGFX flush behavior, on-glass
color/inversion, real touch calibration, perceived latency. Requires the board on
Micro-USB and WiFi credentials in `include/secrets.h` (see
`references/device-api.md`).

1. `make dev-flash` — builds `$(DEV_ENV)` (default `esp32dev_cyd35_uidev`: firmware + dev-tools
   web server), uploads, prints the device IP, exits. Add `PORT=/dev/ttyUSBn` when more than one
   board is plugged in — **the CYD is the CH340**; identify it with `pio device list`.
2. `make dev-touch IP=<ip> X=160 Y=120` — injects a 150 ms touch, in the flashed panel's
   coordinate space. Watch the glass.
3. `make dev-status` — re-query IP/heap/uptime over serial without flashing.

**There are no screenshots on the default 3.5″ board.** Its panel's SDO is unwired, so GRAM
readback returns zeros and `/screenshot.bmp` returns **501** rather than a flawlessly-encoded
black PNG that `dev-shot` would report as a success. The device loop there is `dev-flash` →
`dev-touch` → serial trace → **your eyes**; the sim covers pixels. `make dev-shot IP=<ip>` works
only on the 2.8″: `make dev-flash DEV_ENV=esp32dev_cyd_uidev`, then it fetches
`/screenshot.bmp` → `.pio/sim/device.png`.

Two signatures identify which board is attached, since both are CH340: a **501** from
`/screenshot.bmp` means the 3.5″, and `[ldr] raw=0 auto=0` in the serial trace means the 3.5″'s
**null** ambient-light adapter — *not* a sensor reading zero, which is an easy misread.

The dev-tools server is compiled only under `-D UI_DEV_TOOLS=1` (the `*_uidev` envs);
production (`pio run -e esp32dev_cyd35`) never links WiFi or the web server. Full endpoint
reference: `references/device-api.md`.

## Rules of engagement

- Every feedback step is **one command that exits** — never leave `pio device monitor`
  or a watch loop running.
- UI code changes go in `lib/ui_logic/`; sim harness changes in `sim/`; env changes in
  `platformio.ini` only.
- New `-D`-overridable LVGL toggles follow the `#ifndef` guard idiom in
  `include/lv_conf.h` (see the `LV_USE_TEST` guard) — never reformat that file.
- A screenshot proves rendering, not behavior — keep asserting behavior in
  `test/test_ui_cyd` (see three-tier-testing).

## Code architecture (how screens are structured)

Full patterns, code snippets, and this-board caveats: `references/architecture.md`. The
load-bearing rules:

- **MVVM via LVGL's Observer/Subject API.** App/domain logic in `lib/app_logic/` (no
  `lv_` calls); view models own `lv_subject_t` state and expose intent methods; views
  only build widget trees and call `lv_*_bind_*`. Subjects are **the only interface**
  between UI and app logic, and the only globals (`extern` in one `subjects.h`).
- **Events route through a captureless-lambda or templated trampoline** into view-model
  methods (`user_data` carries `this` — it's consumed; per-widget payloads go through
  subjects instead).
- **LVGL is not thread-safe.** Arduino `loop()` is the single UI task; future FreeRTOS
  tasks (heater, sensors, WiFi) never call `lv_*` — they marshal data into `loop()` via
  queue/volatile (gateway pattern, as `src_cyd/ui_dev_tools.cpp` does for injected touch).
- **All theming goes through `theme.h`'s `apply_*`/`add_*` helpers** — design tokens in, no
  colour literals anywhere else (`rg 'lv_color_hex' lib/ui_logic` should only ever hit
  `theme.h`). Styling is inline per-widget, *not* shared `lv_style_t` globals; the reason and
  the escape hatch are in architecture.md.
- **A custom draw callback MUST clip to `layer->_clip_area`** — correctness, not optimisation,
  on a PSRAM-less partial-render display, and the simulator will never warn you. Cost the dot
  matrix ~3 s per screen once; see architecture.md.
- **Screens: create-on-demand, state in subjects** (a recreated screen re-reads current
  state), delete on leave — this board has no PSRAM to hoard screens in.

## Design rules (pass/fail for every screen)

Physical reality: two resistive panels — 3.5″ 320×480 at **6.49 px/mm** (default) and 2.8″
320×240 at **5.6 px/mm**. Author sizes in mm via `theme.h`, never in px. Operated
with nitrile gloves. Resistive = single-touch, pressure-driven, no hover. Full rationale,
standards, and patterns: `references/design-guide.md`. The hard rules:

- **Touch targets in mm — never px.** Primary controls (Start/Stop/steppers) 12–15 mm;
  absolute floor 10 mm even for secondary controls; spacing ≥2 mm, considerably more between
  benign and hazardous controls. Enlarge and isolate anything that starts heat or UV. Use the
  `theme.h` tokens (`TOUCH_MIN`, `STEPPER_BTN`) rather than converting by hand: the same
  millimetre is a different pixel count on each panel (10 mm = 56 px on the 2.8″, 65 px on the
  default 3.5″), which is the entire reason the tokens exist.
- **No gestures.** Single discrete taps only — no pinch, no multi-touch, drags only as a
  last resort. Sliders need a large handle *plus* +/− buttons; prefer steppers/keypads.
- **Numeric input is constrained.** Steppers (+/−) with enforced min/max and
  disabled-at-limit buttons for values near a default; on-screen keypad for wide ranges;
  never free text. Always show current value with units; sensible defaults.
- **Feedback within 100 ms.** Every tap shows a visible pressed-state change
  immediately; >1 s operations show a working indicator; >10 s (heating, curing) show a
  countdown or percent. A tap with no visible reaction within 100 ms is a defect.
  **A press is a fact, not a transition** — `theme.cpp` enters the pressed state with a
  near-instant 1 ms transition (NOT literally 0 ms: a 0 ms LVGL style transition resolves to the
  *start* value and completes without invalidating, so the pressed fill never repaints until the
  next unrelated invalidation — on a static screen that's the release, and the tile only lights up
  after the finger lifts; see the `ensure_feedback_transitions` comment) and eases back out over
  ~120 ms, and cancels `lv_theme_default`'s press *grow* per-button (a `transform` makes LVGL
  snapshot the widget to an intermediate layer, which on a big tile costs far more than the fill
  change it decorates).
- **Neutral base, color = "look here now"** (ISA-101/IEC 63303). Red = danger/alarm
  (hot surface, UV on, fault); amber = warning (heating, door open); green sparingly.
  **Never color alone** — always pair with a word or icon ("UV ON", "HOT 210 °C"). This is not
  a nicety: a saturated hue on near-black cannot reach the 7:1 critical floor (the reserved
  hues measure ~5.2–5.7:1; `theme.h` carries the table), so the word and glyph are what
  actually carry the state. Contrast floors: ≥4.5:1 body, ≥7:1 critical readouts — readouts are
  `TEXT` and clear it; check any *new* token against **`SURFACE`**, the worse of the two
  grounds. No thin font weights.
  This project's answer is **"Azure Instrument"** (design.md §14): a near-black canvas, the
  three reserved state hues, and **one non-state accent** carrying structure and live data.
  Tokens live in `theme.h` and **nowhere else**; never reach for the accent to mean a state,
  or a state hue to decorate. Pick any new accent for **distance from green/amber/red**, not
  for looks, and judge it on `--screen alerts` (→ design-guide.md for why).
- **Corner brackets mean "this is a touch target" — and nothing else.** `add_brackets()`
  draws from `LV_OBJ_FLAG_CLICKABLE`, the same flag the click routing reads, so the
  affordance cannot drift from the behaviour; disabled controls lose their edge *and* their
  brackets and recede (the opposite of LVGL's default, which brightens them). Not everything
  that consumes a tap is a target: a list row is *selected* and the control you press is the
  footer's Open, so rows get none. Note `lv_obj_create` sets `LV_OBJ_FLAG_CLICKABLE` on
  **every** object — a non-interactive widget must `lv_obj_remove_flag` it, or it silently
  swallows taps and claims an affordance it does not have.
- **Structure is drawn with borders, never glyphs** — there are no box-drawing characters in
  the font, and borders scale with the mm tokens where a glyph would be frozen at one pixel
  size. **Display strings are ASCII + `°` + the icons `lib/ui_logic/fonts/README.md` lists** —
  an em-dash or `§` renders as a missing-glyph box, and the icon sets differ by size
  (`big_font()` has `✓ ✗ ⌫` but *not* `⚠`, so `LV_SYMBOL_WARNING` must stay at the body size).
- **Hazardous starts get specific confirmations.** "Start reflow — heater to 245 °C?"
  with the verb on the button ("Start Heating"), safe option as the default focus, red
  reserved for the hazardous action. Press-and-hold or arm-then-start for the
  highest-energy actions. Keep confirmations rare so they retain force.
- **STOP is always available.** Large, persistent, red STOP on every running screen;
  the UI must never block it (machine work never runs on the LVGL loop). On-screen stop
  complements the hardware thermal fuse — it never replaces hardware protection
  (README safety constraint).
- **Machine state is always visible.** Idle/heating/hot/curing/fault shown prominently
  and persistently — the operator must never think UV is off when it is on.
- **One primary job per screen.** The most important datum (current temp, time
  remaining) is the biggest element. Hub-and-spoke navigation: home status → mode →
  setup → confirm → run/monitor. Consistent header (title + machine state) and footer
  (Back + Stop). Progressive disclosure for rare settings.
- **Live data: big numbers first.** Large numeric readout with units; small
  setpoint-vs-actual trend for reflow; progress bar + time remaining for cure. No 3D,
  no gradients, no gauges without a numeric readout.

## When NOT to use this skill

- Writing or debugging UI unit tests, deciding which test tier covers new code, or a
  native build failing on ESP-IDF/Arduino headers → **three-tier-testing**.
- Blank/garbled screen, wrong colors on glass, dead touch → **hardware-bringup**.
- Editor false errors in firmware code → **clangd-xtensa-setup**.
