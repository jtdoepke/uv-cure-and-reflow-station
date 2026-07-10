---
name: ui-development
description: This skill should be used when designing or iterating on UI screens for the oven controller — creating a new screen, choosing layout/colors/touch-target sizes, adding controls for heat or UV, "take a screenshot of the UI", "what does the screen look like", "click the button and show me", verifying layout/color/spacing changes after editing lib/ui_logic, capturing screenshots / injecting touches on the physical board over WiFi, or structuring UI code — view models, lv_subject_t observer bindings, screen navigation, styles/themes, wiring LVGL event callbacks to C++. Not for writing UI unit tests (see three-tier-testing) or display hardware faults (see hardware-bringup).
---

# UI Development: Iterate Visually, Design for Gloves

Three concerns: **the loop** (how to see and drive the UI while editing it), **the design
rules** (what every screen must satisfy — this is a glove-operated hazardous machine, not
a phone app), and **the code architecture** (how screens are structured so they stay
host-testable). Check new/changed screens against the design rules before calling them
done.

## The host loop (default — seconds per iteration, no hardware)

1. Edit UI code in `lib/ui_logic/` (never `src/main.cpp`; see three-tier-testing).
2. `make sim-shot ARGS="<actions>"` — builds the `native_sim` env, renders the real
   `create_main_ui()` widgets headlessly, runs the scripted actions, writes a PNG.
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

Examples:

- Just look: `make sim-shot`
- Press the demo button, settle, look: `make sim-shot ARGS="click 160 120 wait 300"`
- Before/after pair: `make sim-shot ARGS="shot .pio/sim/before.png click 160 120 wait 300"`
- Custom output: `make sim-shot SIM_OUT=.pio/sim/settings.png ARGS="click 40 210"`

Coordinates are the same 320×240 landscape space the `native_ui` tests use
(`lv_test_mouse_click_at`). The sim renders RGB565 exactly as the device does (same
`lv_conf.h`, same color depth), so colors/dithering match the firmware rasterizer.

The harness lives in `sim/sim_main.cpp` (a host CLI, not firmware, not a test suite).
Exit codes: 0 ok, 1 usage error, 2 PNG write failure; prints `WROTE <path>` per capture.

## The device loop (escalation — real pixels on glass)

Escalate only for what the dummy renderer cannot show: LovyanGFX flush behavior, on-glass
color/inversion, real touch calibration, perceived latency. Requires the board on
Micro-USB and WiFi credentials in `include/secrets.h` (see
`references/device-api.md`).

1. `make dev-flash` — builds `esp32dev_uidev` (firmware + dev-tools web server), uploads,
   prints the device IP, exits.
2. `make dev-shot IP=<ip>` — fetches `/screenshot.bmp` (live ST7789 GRAM readback),
   converts to `.pio/sim/device.png`. Read it.
3. `make dev-touch IP=<ip> X=160 Y=120` — injects a 150 ms touch at screen coords.
4. `make dev-status` — re-query IP/heap/uptime over serial without flashing.

The dev-tools server is compiled only under `-D UI_DEV_TOOLS=1` (the `esp32dev_uidev`
env); production `pio run -e esp32dev` never links WiFi or the web server. Full endpoint
reference: `references/device-api.md`.

## Rules of engagement

- Every feedback step is **one command that exits** — never leave `pio device monitor`
  or a watch loop running.
- UI code changes go in `lib/ui_logic/`; sim harness changes in `sim/`; env changes in
  `platformio.ini` only.
- New `-D`-overridable LVGL toggles follow the `#ifndef` guard idiom in
  `include/lv_conf.h` (see the `LV_USE_TEST` guard) — never reformat that file.
- A screenshot proves rendering, not behavior — keep asserting behavior in
  `test/test_ui` (see three-tier-testing).

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
  queue/volatile (gateway pattern, as `src/ui_dev_tools.cpp` does for injected touch).
- **Shared `static lv_style_t` + design tokens** for theming; styles must outlive every
  widget referencing them; `LV_STYLE_CONST_INIT` keeps fixed styles in flash.
- **Screens: create-on-demand, state in subjects** (a recreated screen re-reads current
  state), delete on leave — this board has no PSRAM to hoard screens in.

## Design rules (pass/fail for every screen)

Physical reality: 2.8″ 320×240 resistive panel ≈ **0.18 mm/px (5.6 px/mm)**, operated
with nitrile gloves. Resistive = single-touch, pressure-driven, no hover. Full rationale,
standards, and patterns: `references/design-guide.md`. The hard rules:

- **Touch targets in mm, converted to px.** Primary controls (Start/Stop/steppers)
  12–15 mm ≈ **67–84 px**; absolute floor 10 mm ≈ **56 px** even for secondary controls;
  spacing ≥2 mm ≈ **11 px**, considerably more between benign and hazardous controls.
  Enlarge and isolate anything that starts heat or UV.
- **No gestures.** Single discrete taps only — no pinch, no multi-touch, drags only as a
  last resort. Sliders need a large handle *plus* +/− buttons; prefer steppers/keypads.
- **Numeric input is constrained.** Steppers (+/−) with enforced min/max and
  disabled-at-limit buttons for values near a default; on-screen keypad for wide ranges;
  never free text. Always show current value with units; sensible defaults.
- **Feedback within 100 ms.** Every tap shows a visible pressed-state change
  immediately; >1 s operations show a working indicator; >10 s (heating, curing) show a
  countdown or percent. A tap with no visible reaction within 100 ms is a defect.
- **Grayscale base, color = "look here now"** (ISA-101/IEC 63303). Red = danger/alarm
  (hot surface, UV on, fault); amber = warning (heating, door open); green sparingly.
  **Never color alone** — always pair with a word or icon ("UV ON", "HOT 210 °C").
  Contrast ≥4.5:1 body text, ≥7:1 critical readouts; no thin font weights.
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
