// NvsSettingsStorage — the ISettingsStorage firmware adapter, backing the settings blob with
// ESP32 NVS via the Arduino Preferences library. One blob under a fixed namespace/key; the store
// (lib/app_logic/settings_store.h) owns the byte layout, so this stays dumb.
//
// Firmware-only: it #includes <Preferences.h> (Arduino/ESP-IDF), so — like the LGFX glue — it
// lives in src_cyd/ and never compiles for the native test targets. The host tests drive the
// store through FakeSettingsStorage instead.
#pragma once

#include <Preferences.h>

#include "ISettingsStorage.h"

class NvsSettingsStorage : public ISettingsStorage {
public:
  size_t load(uint8_t *buf, size_t cap) override {
    Preferences prefs;
    if (!prefs.begin(kNamespace, /*readOnly=*/true)) {
      return 0; // namespace doesn't exist yet (first boot)
    }
    size_t len = prefs.getBytesLength(kKey);
    size_t n = (len > 0 && len <= cap) ? prefs.getBytes(kKey, buf, cap) : 0;
    prefs.end();
    return n;
  }

  bool save(const uint8_t *buf, size_t len) override {
    Preferences prefs;
    if (!prefs.begin(kNamespace, /*readOnly=*/false)) {
      return false;
    }
    size_t written = prefs.putBytes(kKey, buf, len);
    prefs.end();
    return written == len;
  }

private:
  // NVS keys/namespaces are capped at 15 chars.
  static constexpr const char *kNamespace = "oven-settings";
  static constexpr const char *kKey = "blob";
};
