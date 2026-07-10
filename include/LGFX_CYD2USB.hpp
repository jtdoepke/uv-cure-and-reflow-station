// LovyanGFX config for the dual-USB ESP32-2432S028 v3 "Cheap Yellow Display" (ST7789).
// Values from LovyanGFX's autodetect table for the Sunton 2432S028 board.
// If the screen stays blank, try cfg.spi_mode = 3 (some units need it).
#pragma once
#include <LovyanGFX.hpp>

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel_instance;   // ST7789 for the 2-USB board
  lgfx::Bus_SPI _bus_instance;
  lgfx::Light_PWM _light_instance;
  lgfx::Touch_XPT2046 _touch_instance;

public:
  LGFX(void) {
    { // SPI bus (display) -> HSPI / SPI2
      auto cfg = _bus_instance.config();
      cfg.spi_host = HSPI_HOST; // == SPI2_HOST on classic ESP32
      cfg.spi_mode = 0;         // try 3 if the screen stays blank/garbled
      cfg.freq_write = 80000000;
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
      cfg.pin_rst = -1;
      cfg.pin_busy = -1;
      cfg.panel_width = 240;
      cfg.panel_height = 320;
      cfg.memory_width = 240;
      cfg.memory_height = 320;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0; // 2-USB ST7789 = 0 (ILI9341 board uses 2)
      cfg.dummy_read_pixel = 16;
      cfg.dummy_read_bits = 1;
      cfg.readable = true;
      cfg.invert = false;    // 2-USB ST7789: false gives correct colors
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      _panel_instance.config(cfg);
    }
    { // Backlight
      auto cfg = _light_instance.config();
      cfg.pin_bl = 21;
      cfg.invert = false;
      cfg.freq = 12000;
      cfg.pwm_channel = 7;
      _light_instance.config(cfg);
      _panel_instance.setLight(&_light_instance);
    }
    { // Touch (XPT2046) on its own software-SPI bus (separate from the display bus)
      auto cfg = _touch_instance.config();
      cfg.x_min = 240;
      cfg.x_max = 3800;
      cfg.y_min = 3700; // y_min > y_max is intentional (axis flip)
      cfg.y_max = 200;
      // pin_int MUST be -1 on this board. LovyanGFX's touch read bails out whenever the
      // IRQ pin reads high; the CYD wires T_IRQ to GPIO36 (input-only, no internal pull)
      // with no external pull-up, so polling it is unreliable and blocks all touch reads.
      // With -1, LovyanGFX reads pressure over SPI directly on every poll. (Verified on HW.)
      cfg.pin_int = -1;
      cfg.bus_shared = false;
      cfg.offset_rotation = 2;
      cfg.spi_host = -1; // software SPI on the touch pins
      cfg.freq = 1000000;
      cfg.pin_sclk = 25;
      cfg.pin_mosi = 32;
      cfg.pin_miso = 39;
      cfg.pin_cs = 33;
      _touch_instance.config(cfg);
      _panel_instance.setTouch(&_touch_instance);
    }
    setPanel(&_panel_instance);
  }
};
