// LittleFsSettingsStorage — the ISettingsStorage firmware adapter for the controller (design.md
// §7/§24; Wave R2b of the §2 "CYD is a UI remote" split). The single-blob sibling of
// LittleFsProfileStorage: one fixed file ("/settings.bin") holding the whole settings blob.
// control::SettingsStore owns the byte layout, so this stays dumb load/save.
//
// Firmware-only (#includes <LittleFS.h>), so it lives in src_control/ and never compiles for the
// native test targets; the host tests drive the store through FakeSettingsStorage. Reuses the
// LittleFS mount main.cpp already brings up for the profile library.
#pragma once

#include <Arduino.h>
#include <LittleFS.h>

#include "ISettingsStorage.h"

class LittleFsSettingsStorage : public ISettingsStorage {
public:
  explicit LittleFsSettingsStorage(const char *path = "/settings.bin") : path_(path) {}

  size_t load(uint8_t *buf, size_t cap) override {
    File f = LittleFS.open(path_, "r");
    if (!f || f.isDirectory()) {
      return 0; // first boot / no blob yet -> store falls back to defaults
    }
    const size_t len = f.size();
    if (len == 0 || len > cap) {
      f.close();
      return 0;
    }
    const size_t n = f.read(buf, len);
    f.close();
    return n == len ? n : 0;
  }

  bool save(const uint8_t *buf, size_t len) override {
    File f = LittleFS.open(path_, "w");
    if (!f) {
      return false;
    }
    const size_t n = f.write(buf, len);
    f.close();
    return n == len;
  }

private:
  const char *path_;
};
