// UI dev tools (screenshot + touch-injection HTTP API) — compiled only in the
// esp32dev_cyd_uidev env (-D UI_DEV_TOOLS=1). See the ui-development skill.
#pragma once

#if defined(UI_DEV_TOOLS)

#include <cstdint>

class LGFX;

// Connect WiFi (include/secrets.h) and start the dev web server. Call at end of setup().
void ui_dev_tools_begin(LGFX &gfx);

// Service HTTP + the serial STATUS command. Call every loop() iteration.
void ui_dev_tools_loop();

// True while an injected touch is active; fills the screen coords. The LVGL touch-read
// callback checks this before the real touch controller.
bool ui_dev_touch_get(int16_t *x, int16_t *y);

#endif // UI_DEV_TOOLS
