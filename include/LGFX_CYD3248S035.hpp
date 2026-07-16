// LovyanGFX config for the 3.5" ESP32-3248S035 "Cheap Yellow Display" (ST7796S + XPT2046).
//
// Three differences from the 2.8" board (LGFX_CYD2432S028.hpp) carry all the risk:
//
//   - ST7796S, 320x480 native portrait (vs ST7789, 240x320). LovyanGFX ships Panel_ST7796, so
//     this is a driver swap, not a new dependency.
//   - The display bus is VSPI/SPI3 (vs HSPI/SPI2) — the touch is on the same bus here, and the
//     2.8"'s HSPI choice was only ever about keeping the two apart.
//   - Touch SHARES the display's hardware SPI bus (the 2.8" gives touch its own bit-banged
//     software-SPI bus on 25/32/39/33). Hence bus_shared = true on BOTH the panel and touch cfg:
//     Panel_Device::getTouchRaw only ends and reopens the display transaction around a touch read
//     when it is told the bus is shared, and getting that wrong corrupts the panel exactly when
//     someone touches it. The sharing is also *why* GPIO25/32 are free for the link UART (§2).
//
// Verified on the bench (see the hardware-bringup skill for the runbook):
//   - The ESP32 is a D0WD-V3, package 1, 4 MB external flash, no PSRAM — i.e. a WROOM-32E-class
//     module. NOT an ESP32-S3, despite the widely-copied community config for this board being
//     written for one; its GPIO numbers happen to be valid here, but nothing else about it is.
//   - The XPT2046 answers on the shared bus with CS 33 and MISO 12.
//   - The panel drives 320x480 at rotation 0 (portrait), full screen, no offsets — which is how
//     we know it is a 320x480 ST7796S at all: it answers every ID read with zeros (see readable).
//   - invert=false / rgb_order=false, read off labelled colour bars through the real LVGL flush.
//   - The link on GPIO25/32 handshakes with the controller (matched=1), so those pins are right.
#pragma once
#include <LovyanGFX.hpp>

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7796 _panel_instance;
  lgfx::Bus_SPI _bus_instance;
  lgfx::Light_PWM _light_instance;
  lgfx::Touch_XPT2046 _touch_instance;

public:
  LGFX(void) {
    { // SPI bus (display + touch share it) -> VSPI / SPI3
      auto cfg = _bus_instance.config();
      cfg.spi_host = VSPI_HOST; // == SPI3_HOST on classic ESP32
      cfg.spi_mode = 0;         // try 3 if the screen stays blank/garbled
      // 40 MHz to start: the 2.8" runs 80, but this bus also carries a 2 MHz-max XPT2046 and
      // LovyanGFX re-clocks per transaction. TODO(HW): try 80 MHz once the panel is stable.
      cfg.freq_write = 40000000;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = 14;
      cfg.pin_mosi = 13;
      cfg.pin_miso = 12;
      cfg.pin_dc = 2;
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }
    { // Panel
      auto cfg = _panel_instance.config();
      cfg.pin_cs = 15;
      cfg.pin_rst = 4; // wired here, unlike the 2.8" (-1)
      cfg.pin_busy = -1;
      cfg.panel_width = 320;
      cfg.panel_height = 480;
      cfg.memory_width = 320;
      cfg.memory_height = 480;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.dummy_read_pixel = 8; // Panel_ST7796's own default (the 2.8"'s ST7789 wants 16)
      cfg.dummy_read_bits = 1;
      // The panel answers every ID read (RDDID/RDID4) with zeros on a MISO line the XPT2046
      // demonstrably drives, so its SDO appears not to be wired — common on these boards, and it
      // means GRAM readback returns nothing. Consequence: `make dev-shot` (UI_DEV_TOOLS'
      // /screenshot.bmp) cannot work on this board; use the simulator. TODO(HW): revisit if a
      // schematic turns up — this is inferred from behaviour, not from a datasheet.
      cfg.readable = false;
      // Verified on the bench: true renders every colour as its exact complement (red->cyan,
      // white->black). Same as the 2.8" board, contrary to the folklore that ST7796S CYDs want
      // true. Worth knowing when tuning this: LGFX_Device::init_impl calls
      // invertDisplay(getInvert()) i.e. setInvert(false), and setInvert writes
      // (arg ^ cfg.invert) ? INVON : INVOFF — so cfg.invert=true means init leaves the panel
      // INVERTED, and gfx.invertDisplay(x) reads as "x XOR this flag", not as an absolute.
      cfg.invert = false;
      cfg.rgb_order = false; // verified: RED renders red, not blue
      cfg.dlen_16bit = false;
      cfg.bus_shared = true; // touch is on this bus — see the header comment
      _panel_instance.config(cfg);
    }
    { // Backlight
      auto cfg = _light_instance.config();
      cfg.pin_bl = 27; // the 2.8" uses 21; 27 is that board's link TX (§2)
      cfg.invert = false;
      cfg.freq = 12000;
      cfg.pwm_channel = 7;
      _light_instance.config(cfg);
      _panel_instance.setLight(&_light_instance);
    }
    { // Touch (XPT2046) — on the display's hardware SPI bus, not a software one
      auto cfg = _touch_instance.config();
      // Calibrated on the unit at rotation 0 with tools/touch_calibrate (15-point grid, least
      // squares per axis), then evaluated at the endpoints LovyanGFX maps: x_min/x_max are raw X
      // at screen x = 0 / panel_width-1, y_min/y_max raw Y at y = 0 / panel_height-1.
      // Residual over the grid: mean 2.8 px in X, 3.2 px in Y; worst 8.7 px — against a 65 px
      // (10 mm) touch floor.
      //
      // Preferred over LovyanGFX's own calibrateTouch(), which samples the four true corners
      // (LGFXBase.cpp draws them at width()-1 / height()-1, i.e. half behind the bezel): tapping
      // those biases inward, and its 4 points leave no redundancy and no measurable residual.
      //
      // The residual is not slop in the fit, it is the panel: the X slope runs 10.4 raw/px on the
      // left half vs 11.5 on the right, and raw X drifts ~65 counts with Y. That cross-coupling
      // is unrepresentable in this axis-aligned min/max model — LovyanGFX's setTouchCalibrate()
      // affine could hold it, at the cost of storing 8 magic numbers instead of 4 legible ones.
      // Revisit only if a target ever needs better than ~9 px.
      cfg.x_min = 147;
      cfg.x_max = 3786;
      cfg.y_min = 4036; // y_min > y_max is the intentional axis flip (as on the 2.8") — do not
      cfg.y_max = 315;  // "fix" these into ascending order
      // pin_int MUST be -1, as on the 2.8": LovyanGFX's touch read bails out whenever it polls
      // the IRQ pin high, and T_IRQ is GPIO36 — input-only, no internal pull. Verified here: the
      // pin reads high while untouched, so polling it would block every read.
      cfg.pin_int = -1;
      cfg.bus_shared = true;
      // 0 verified: raw X tracks screen X and raw Y screen Y (both calibration spans came out
      // wide — ~3316 and ~3484; a transposed axis shows up as one span collapsing).
      cfg.offset_rotation = 0;
      cfg.spi_host = VSPI_HOST;
      cfg.freq = 1000000; // XPT2046 tops out ~2 MHz
      cfg.pin_sclk = 14;
      cfg.pin_mosi = 13;
      cfg.pin_miso = 12;
      cfg.pin_cs = 33;
      _touch_instance.config(cfg);
      _panel_instance.setTouch(&_touch_instance);
    }
    setPanel(&_panel_instance);
  }
};
