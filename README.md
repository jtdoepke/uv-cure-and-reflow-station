# Oven Controller — UV Curing Station & PCB Reflow Oven

Firmware for turning a retired microwave / air-fryer combo oven into a **dual-purpose
bench appliance**, controlled by an ESP32 "Cheap Yellow Display" (CYD) touchscreen that
replaces the oven's original control panel.

Two jobs from one box:

- **UV curing station** — post-cure SLA/DLP 3D-resin prints with a UV LED array on a timer.
- **PCB reflow oven** — run solder-paste reflow temperature profiles using the oven's
  existing heating element and a thermocouple.

A CYD ("Cheap Yellow Display") ESP32 touchscreen provides the UI: pick a mode, set
time/temperature, run a profile, and watch live status — all on a panel mounted where the
microwave keypad used to be. A second ESP32 owns the safety-critical outputs.

> ⚠️ **This project involves mains voltage, high temperatures, and UV light. Read the
> [Safety](#safety) section before building anything. Mains wiring can kill you; a
> mis-controlled heating element is a fire hazard; UV damages eyes and skin.**

---

## Status

**Early firmware scaffold — display and touch verified; no oven control yet.**

What works today (flashed and confirmed on hardware):

- LovyanGFX + LVGL 9.5 driving the ST7789 display and XPT2046 touch on the CYD.
- A boot-time color self-test (RED → GREEN → BLUE → WHITE) to confirm the panel.
- A demo UI: a "Hello CYD!" label and a touch-counting button.

Not built yet: heater/UV switching, temperature sensing, timers, reflow profiles, the mode
UI, or any connection to the oven's electrics. See the [Roadmap](#roadmap).

## Hardware

### HMI — a CYD "Cheap Yellow Display" (two variants supported)

Both are ESP32-WROOM-32 (no PSRAM) with **XPT2046** resistive touch. Pick one with a build
env; nothing under `lib/` knows which is fitted.

| | **ESP32-3248S035** (default) | ESP32-2432S028 |
|---|---|---|
| Panel | 3.5" **ST7796S**, 320×480 portrait | 2.8" **ST7789**, 240×320 run landscape |
| Env | `esp32dev_cyd35` | `esp32dev_cyd` |
| LGFX config | [`include/LGFX_CYD3248S035.hpp`](include/LGFX_CYD3248S035.hpp) | [`include/LGFX_CYD2432S028.hpp`](include/LGFX_CYD2432S028.hpp) |
| Ambient-light sensor | none (Settings shows a plain brightness control) | LDR on GPIO34 (auto-brightness) |
| GRAM readback | no (SDO unwired — no on-device screenshots) | yes |

- **The 3.5" board is the default because it survives WiFi bring-up.** The 2.8"'s stock
  (likely counterfeit) AMS1117 LDO browns out the moment the radio initializes — and OTA is
  the controller's only field-reflash path once the oven is enclosed, so that is a board with
  no field-update story, not a cosmetic fault. See `docs/design.md` §21.
- The 2.8" is the "7789" v3 board (Micro-USB **and** USB-C ports). On either board flash over
  the **Micro-USB** port — the USB-C port lacks CC resistors and won't enumerate on many hosts.
- Pin maps live in [`include/cyd_board.h`](include/cyd_board.h) + the LGFX headers above;
  build/upload commands in [`CLAUDE.md`](CLAUDE.md).

### The oven (donor appliance)

- A microwave / air-fryer combo. The **microwave magnetron is not used** and should be
  removed/disconnected — only the **resistive heating element(s)** and fan are reused for
  reflow. The original control board and keypad are removed and replaced by the CYD.

### Planned control electronics (not yet designed/wired)

| Function | Likely part | Notes |
|----------|-------------|-------|
| Switch heating element | Solid-state relay (SSR) sized for the element's current | Zero-cross SSR; drive from a CYD GPIO |
| Temperature sensing (reflow) | K-type thermocouple + MAX31855 / MAX6675 (SPI) | Accurate temp is essential for reflow profiles |
| UV LED array | 405 nm LEDs + MOSFET or relay | For resin curing; separate from the heater |
| Door interlock | Switch on the door | Cut power to heater/UV when the door opens |
| Safety cutoff | Thermal fuse + independent over-temp limit | Hardware backstop, not software-only |

**GPIO is tight on the CYD** — most pins go to the display, touch, SD slot, RGB LED, LDR and
speaker. That is why the safety-critical outputs live on a *second* ESP32 (the controller),
which has its own pins to spare; the CYD only speaks to it over a UART. The free pins differ
per board (2.8": GPIO22/27 on CN1; 3.5": GPIO25/32, since its touch shares the display bus and
27 is the backlight) — see `include/cyd_board.h`, and `docs/design.md` §2/§6.

## Getting started

Requires [PlatformIO](https://platformio.org/) (`pio` CLI, or the VS Code extension). If
`pio` isn't installed, run `python get-platformio.py`.

```bash
# Build
pio run

# Flash over the Micro-USB port, then open the serial monitor
pio run -t upload -t monitor
```

On boot you should see the color self-test, then the demo UI; tapping the button increments
its counter. If the screen is blank or colors look wrong, see the display/touch tuning notes
in [`CLAUDE.md`](CLAUDE.md).

### Testing

Tests run in three tiers so most of them need no hardware (details in [`CLAUDE.md`](CLAUDE.md)):

```bash
pio test -e native_logic_cyd   # fast host tests of app logic (no board, no display libs)
pio test -e native_ui_cyd      # LVGL UI tests on a headless host display (no board)
pio test -e embedded       # on the real CYD: display/touch/heap checks (Micro-USB port)
```

The two native suites (plus a firmware compile-check) also run in CI on every push
([`.github/workflows/ci.yml`](.github/workflows/ci.yml)).

### Formatting & linting

Code is auto-formatted with clang-format and checked by pre-commit (whitespace, YAML,
Markdown); CI enforces both. One-time setup:

```bash
pip install pre-commit && pre-commit install   # or: make hooks
mise install                                    # provides the pinned clang-format
```

Then `make format` formats C++ and `make lint` runs every check. See the **Linting &
formatting** section of [`CLAUDE.md`](CLAUDE.md) for details.

## Project structure

```text
platformio.ini            PlatformIO config — CYD + controller firmware, native/embedded test envs
include/
  cyd_board.h             The HMI board's pins, capabilities and orientation (one #if)
  LGFX_CYD3248S035.hpp    LovyanGFX display + touch config — 3.5" ST7796S board
  LGFX_CYD2432S028.hpp    LovyanGFX display + touch config — 2.8" ST7789 board
  lv_conf.h               LVGL 9.5 configuration
lib/                      Testable, hardware-independent modules (compiled by app + tests)
  display_port/           IDisplay / ITouch ports — the hardware boundary
  app_logic/              Pure business logic (e.g. TapCounter) behind the ports
  ui_logic/               LVGL UI construction (no LovyanGFX), host-testable
src_cyd/
  main.cpp                setup()/loop(): hardware bring-up, self-test, then create_main_ui()
test/                     Unity suites: test_logic_cyd / test_ui_cyd (native), test_embedded_hw
CLAUDE.md                 Detailed build notes, board gotchas, and design decisions
```

## Why LovyanGFX?

The display/touch stack is LovyanGFX + LVGL rather than the popular `esp32_smartdisplay`
library. LovyanGFX builds against the current arduino-esp32 / ESP-IDF toolchain with no
version pinning; `esp32_smartdisplay` wraps ESP-IDF internal headers that break on newer
IDF releases. Full rationale is in [`CLAUDE.md`](CLAUDE.md).

## Roadmap

- [ ] Hardware: SSR + heater wiring, thermocouple front-end, UV LED driver, door interlock.
- [ ] Temperature reading and closed-loop (PID) heater control.
- [ ] **UV curing mode:** timer, UV on/off, optional turntable, done alert.
- [ ] **Reflow mode:** editable solder-paste profiles (preheat / soak / reflow / cool),
      live temperature graph, profile progress.
- [ ] Mode-select home screen and settings UI.
- [ ] Safety: software over-temp abort layered on top of the hardware cutoff; watchdog;
      fail-safe outputs (heater/UV default OFF on fault or reset).

## Safety

This is a mains-powered heating and UV appliance built from a modified oven. Treat it as
genuinely dangerous and design for failure.

- **Mains voltage is lethal.** Do not work on the appliance's wiring while plugged in.
  Keep mains-side wiring properly insulated, strain-relieved, and fused; keep it physically
  and electrically separated from the 3.3 V CYD logic (use an SSR/relay with proper isolation,
  never switch mains directly from a GPIO).
- **Fire risk.** A heater stuck on can start a fire. Include an **independent hardware
  over-temperature cutoff and thermal fuse** that does not depend on the ESP32 or firmware.
  Never run the oven unattended.
- **Fail-safe by default.** Heater and UV outputs must default OFF on boot, reset, crash,
  brown-out, or sensor fault. Use a watchdog. Assume the firmware *will* crash at some point.
- **Door interlock.** Cut heater and UV power when the door opens.
- **UV hazard.** 405 nm (and especially any shorter-wavelength) UV damages eyes and skin.
  Enclose the source and interlock it with the door; wear UV-rated protection.
- **Resin & fumes.** Curing resin and reflowing solder paste release fumes — ventilate, and
  keep the curing/reflow functions and their residues separate (don't cure food-adjacent).
- **Remove the magnetron.** If the donor is a microwave, the high-voltage microwave section
  (magnetron, HV transformer, HV capacitor) is extremely dangerous and unused here — have it
  removed/discharged/disconnected by someone who knows how.

This project is provided as-is, with no warranty; you are responsible for building and
operating it safely and legally.

## License

[MIT](LICENSE) © 2026 Jaye Doepke.

Note the "AS IS", no-warranty terms — especially relevant given the mains/heat/UV hazards
described above. You are responsible for building and operating this safely.
