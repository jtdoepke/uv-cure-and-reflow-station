// On-device UI dev tools: HTTP screenshot + touch injection + serial STATUS.
//
// Dev-only (esp32dev_cyd_uidev env, -D UI_DEV_TOOLS=1); production firmware never compiles
// this. Uses the Arduino core's synchronous WebServer so handlers run from loop() on
// the LVGL thread — no locking against lv_timer_handler needed. Screenshots stream the
// ST7789's GRAM row-by-row (readRectRGB; the panel is configured readable), so no
// full-frame buffer is required on this PSRAM-less board.

#if defined(UI_DEV_TOOLS)

#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>

#include "cyd_board.h"
#include "ui_dev_tools.h"

// Gitignored WiFi credentials: WIFI_SSID / WIFI_PASSWORD (see include/secrets.h.example).
#if __has_include("secrets.h")
#include "secrets.h"
#else
#warning "include/secrets.h not found; UI dev tools start without WiFi (serial STATUS only)"
#endif

static WebServer server(80);
static LGFX *s_gfx = nullptr;
static bool s_server_up = false;
static String s_serial_buf;

static volatile int16_t s_touch_x = 0;
static volatile int16_t s_touch_y = 0;
static volatile uint32_t s_touch_until = 0; // millis() deadline; 0 = inactive

bool ui_dev_touch_get(int16_t *x, int16_t *y) {
  if (millis() >= s_touch_until)
    return false;
  *x = s_touch_x;
  *y = s_touch_y;
  return true;
}

static void put_u32le(uint8_t *p, uint32_t v) {
  p[0] = v & 0xFF;
  p[1] = (v >> 8) & 0xFF;
  p[2] = (v >> 16) & 0xFF;
  p[3] = (v >> 24) & 0xFF;
}

// GET /screenshot.bmp — live GRAM readback streamed as a bottom-up 24-bit BMP.
// One row buffer (w*3 bytes). BMP requires 4-byte-aligned rows and this writer emits no padding,
// so it relies on w*3 being a multiple of 4 — true for every panel width we ship (320, 480).
static void handle_screenshot() {
  const int32_t w = s_gfx->width();
  const int32_t h = s_gfx->height();
  const uint32_t row_bytes = static_cast<uint32_t>(w) * 3;
  const uint32_t data_bytes = row_bytes * static_cast<uint32_t>(h);

  uint8_t hdr[54] = {0};
  hdr[0] = 'B';
  hdr[1] = 'M';
  put_u32le(hdr + 2, 54 + data_bytes); // file size
  put_u32le(hdr + 10, 54);             // pixel data offset
  put_u32le(hdr + 14, 40);             // BITMAPINFOHEADER size
  put_u32le(hdr + 18, static_cast<uint32_t>(w));
  put_u32le(hdr + 22, static_cast<uint32_t>(h)); // positive = bottom-up
  hdr[26] = 1;                                   // planes
  hdr[28] = 24;                                  // bits per pixel
  put_u32le(hdr + 34, data_bytes);

  server.setContentLength(54 + data_bytes);
  server.send(200, "image/bmp", "");
  WiFiClient client = server.client();
  client.write(hdr, sizeof(hdr));

  // Sized from the configured panel, not a literal: a wider panel would silently overrun this.
  // test_embedded_hw asserts panel::W == gfx.width(), which is what makes the bound real.
  static uint8_t row[panel::W * 3];
  if (w > panel::W) {
    return; // cannot happen if the board flags match the panel; refuse rather than overrun
  }
  for (int32_t y = h - 1; y >= 0; y--) {
    s_gfx->readRectRGB(0, y, w, 1, row); // fills R,G,B per pixel
    for (int32_t x = 0; x < w; x++) {    // BMP wants B,G,R
      uint8_t r = row[x * 3];
      row[x * 3] = row[x * 3 + 2];
      row[x * 3 + 2] = r;
    }
    client.write(row, row_bytes);
  }
}

// GET /api/touch/simulate?x=&y=[&ms=] — arm an injected touch the LVGL indev picks up.
static void handle_touch() {
  if (!server.hasArg("x") || !server.hasArg("y")) {
    server.send(400, "application/json", "{\"error\":\"x and y are required\"}");
    return;
  }
  long x = server.arg("x").toInt();
  long y = server.arg("y").toInt();
  long ms = server.hasArg("ms") ? server.arg("ms").toInt() : 150;
  if (x < 0 || x >= s_gfx->width() || y < 0 || y >= s_gfx->height()) {
    server.send(400, "application/json", "{\"error\":\"coords out of range\"}");
    return;
  }
  ms = constrain(ms, 50, 2000);
  s_touch_x = static_cast<int16_t>(x);
  s_touch_y = static_cast<int16_t>(y);
  s_touch_until = millis() + static_cast<uint32_t>(ms);
  char buf[80];
  snprintf(buf, sizeof(buf), "{\"ok\":true,\"x\":%ld,\"y\":%ld,\"ms\":%ld}", x, y, ms);
  server.send(200, "application/json", buf);
}

static void handle_info() {
  char buf[192];
  snprintf(buf, sizeof(buf),
           "{\"ip\":\"%s\",\"rssi\":%d,\"heap\":%u,\"uptime_s\":%lu,\"width\":%d,\"height\":%d}",
           WiFi.localIP().toString().c_str(), WiFi.RSSI(), ESP.getFreeHeap(),
           static_cast<unsigned long>(millis() / 1000), s_gfx->width(), s_gfx->height());
  server.send(200, "application/json", buf);
}

// Serial STATUS protocol — framing and keys match mccahan/esp32-display-claude-base so
// tools/uidev_extra_script.py and monitor/filter_get_status.py parse it unchanged.
static void print_status() {
  Serial.println("---STATUS_BEGIN---");
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WIFI:CONNECTED");
    Serial.print("IP:");
    Serial.println(WiFi.localIP().toString());
    Serial.print("SSID:");
    Serial.println(WiFi.SSID());
  } else {
    Serial.println("WIFI:DISCONNECTED");
  }
  Serial.print("HEAP:");
  Serial.println(ESP.getFreeHeap());
  Serial.print("UPTIME:");
  Serial.println(millis() / 1000);
  Serial.println("---STATUS_END---");
}

void ui_dev_tools_begin(LGFX &gfx) {
  s_gfx = &gfx;

#if defined(WIFI_SSID)
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("ui_dev_tools: connecting to WiFi");
  uint32_t deadline = millis() + 15000;
  while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    server.on("/screenshot.bmp", HTTP_GET, handle_screenshot);
    server.on("/api/touch/simulate", HTTP_GET, handle_touch);
    server.on("/api/info", HTTP_GET, handle_info);
    server.begin();
    s_server_up = true;
    Serial.print("READY ip=");
    Serial.println(WiFi.localIP().toString());
  } else {
    Serial.println("ui_dev_tools: WiFi connect timed out; serial STATUS only");
  }
#else
  Serial.println("ui_dev_tools: no include/secrets.h; serial STATUS only");
#endif
}

void ui_dev_tools_loop() {
  if (s_server_up)
    server.handleClient();
  while (Serial.available() > 0) {
    char c = static_cast<char>(Serial.read());
    if (c == '\n' || c == '\r') {
      if (s_serial_buf == "STATUS")
        print_status();
      s_serial_buf = "";
    } else {
      s_serial_buf += c;
    }
  }
}

#endif // UI_DEV_TOOLS
