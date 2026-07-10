# Oven Controller — UV Curing Station & PCB Reflow Oven

Firmware for turning a retired microwave / air-fryer combo oven into a **dual-purpose
bench appliance**, controlled by an ESP32 "Cheap Yellow Display" (CYD) touchscreen that
replaces the oven's original control panel.

Two jobs from one box:

- **UV curing station** — post-cure SLA/DLP 3D-resin prints with a UV LED array on a timer.
- **PCB reflow oven** — run solder-paste reflow temperature profiles using the oven's
  existing heating element and a thermocouple.

The CYD (ESP32-2432S028, 320×240 ST7789 + resistive touch) provides the UI: pick a mode,
set time/temperature, run a profile, and watch live status — all on the touchscreen mounted
where the microwave keypad used to be.

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

### Controller — ESP32-2432S028 "Cheap Yellow Display" (dual-USB v3)

- ESP32-WROOM-32 (no PSRAM), 320×240 **ST7789** SPI TFT, **XPT2046** resistive touch.
- This is the "7789" v3 board (Micro-USB **and** USB-C ports). Flash over the **Micro-USB**
  port — the USB-C port lacks CC resistors and won't enumerate on many hosts.
- Firmware/pin details live in [`CLAUDE.md`](CLAUDE.md) and
  [`include/LGFX_CYD2USB.hpp`](include/LGFX_CYD2USB.hpp).

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

**GPIO is tight on the CYD.** Most pins are taken by the display, touch, SD slot, RGB LED,
LDR, and speaker. Spare pins are exposed on the CN1/P3/P5 headers (commonly GPIO 22, 27, and
input-only 35, with I²C available on 22/27). Expect to drive the SSR/UV/thermocouple over
those few pins and/or an I²C GPIO expander. This is an open design question.

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
pio test -e native_logic   # fast host tests of app logic (no board, no display libs)
pio test -e native_ui      # LVGL UI tests on a headless host display (no board)
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
platformio.ini            PlatformIO config — esp32dev firmware + native/embedded test envs
include/
  LGFX_CYD2USB.hpp        LovyanGFX display + touch configuration for this board
  lv_conf.h               LVGL 9.5 configuration
lib/                      Testable, hardware-independent modules (compiled by app + tests)
  display_port/           IDisplay / ITouch ports — the hardware boundary
  app_logic/              Pure business logic (e.g. TapCounter) behind the ports
  ui_logic/               LVGL UI construction (no LovyanGFX), host-testable
src/
  main.cpp                setup()/loop(): hardware bring-up, self-test, then create_main_ui()
test/                     Unity suites: test_logic / test_ui (native), test_embedded_hw
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
