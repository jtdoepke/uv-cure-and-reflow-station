# On-Device UI Dev API (esp32dev_uidev env)

Real pixels on glass: the `esp32dev_uidev` PlatformIO env is the production firmware
plus `src/ui_dev_tools.cpp` (compiled only under `-D UI_DEV_TOOLS=1` — production
`pio run -e esp32dev` never links WiFi or the web server). The startup color self-test
is skipped in this env to keep flash-iterate cycles short.

## One-time setup

1. `cp include/secrets.h.example include/secrets.h` and fill in `WIFI_SSID` /
   `WIFI_PASSWORD` (2.4 GHz network; the file is gitignored). Without it the firmware
   boots with serial STATUS only, no web server.
2. Board on the **Micro-USB** port (USB-C won't enumerate; see hardware-bringup).

## The loop

1. `make dev-flash` — builds `esp32dev_uidev`, uploads, waits for boot, prints the
   STATUS block including `IP Address:` (via `tools/uidev_extra_script.py`), exits.
2. `make dev-shot IP=<ip>` — fetches `/screenshot.bmp`, converts to
   `.pio/sim/device.png` (`tools/cyd-shot.sh` → `tools/bmp2png.py`, stdlib only). Read
   the PNG.
3. `make dev-touch IP=<ip> X=160 Y=120` — injects a 150 ms touch, then re-shot to see
   the effect.
4. `make dev-status` — re-query IP/heap/uptime over serial without flashing
   (`pio run -e esp32dev_uidev -t status`).

Every command exits on its own — never leave a bare `pio device monitor` running. If a
monitor is unavoidable, use the auto-exiting filter:
`pio device monitor -e esp32dev_uidev -f get_status`.

## HTTP endpoints

| Endpoint | Effect |
|---|---|
| `GET /screenshot.bmp` | Live ST7789 GRAM readback, streamed row-by-row as a bottom-up 24-bit BMP (~230 KB). No capture step — every GET reads the panel now. |
| `GET /api/touch/simulate?x=&y=[&ms=]` | Arm an injected touch at screen coords (320×240 landscape, same space as the simulator). `ms` clamped 50–2000, default 150. The LVGL indev callback reports it pressed until expiry. |
| `GET /api/info` | JSON: ip, rssi, free heap, uptime, panel dimensions. |

## Serial STATUS protocol

Send `STATUS\n` at 115200 baud; the reply is framed for machine parsing (format shared
with mccahan/esp32-display-claude-base so its tooling works unchanged):

```text
---STATUS_BEGIN---
WIFI:CONNECTED
IP:192.168.1.23
SSID:mynet
HEAP:123456
UPTIME:42
---STATUS_END---
```

`tools/uidev_extra_script.py` parses this after every upload (and for `-t status`);
`monitor/filter_get_status.py` does the same as a monitor filter.

## Implementation notes / gotchas

- **Screenshots read the panel's GRAM**, not an LVGL buffer — there is no full
  framebuffer on this PSRAM-less board (partial 1/10-screen draw buffers only). The
  panel is configured readable in `include/LGFX_CYD2USB.hpp` (`cfg.readable = true`,
  MISO 12, `freq_read` 16 MHz). If a capture ever comes back garbled, lower
  `freq_read` before suspecting anything else.
- The web server is the Arduino core's **synchronous** `WebServer`; handlers run from
  `loop()` on the LVGL thread, so screenshot/touch handlers can't race
  `lv_timer_handler`. A screenshot blocks the UI for its duration (~1–2 s) — that's
  fine for a dev tool, don't "fix" it with an async server.
- Touch injection is the volatile-globals pattern: `/api/touch/simulate` arms
  coords+deadline; `my_touch_read` in `src/main.cpp` checks `ui_dev_touch_get()` before
  the real `gfx.getTouch()`.
- No OTA: USB flashing is already a single non-interactive command and the first flash
  needs USB anyway. If the uidev image ever outgrows the default app partition, set
  `board_build.partitions = huge_app.csv` in the `esp32dev_uidev` env only.
