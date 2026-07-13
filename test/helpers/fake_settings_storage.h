// In-memory fake for ISettingsStorage — injected into SettingsStore under the native_logic_cyd
// env so persistence round-trips are tested with no NVS/flash. Header-only, shared via
// `#include "helpers/fake_settings_storage.h"` (mirrors fake_clock.h / fake_heater_switch.h).
#pragma once

#include <cstring>
#include <vector>

#include "ISettingsStorage.h"

// Holds the last-saved blob in a byte vector and counts saves, so tests can assert both the
// round-trip and that save() actually fired. `present` starts false to model a blank device;
// tests can also poke `blob` directly to model a corrupt/stale/foreign blob.
struct FakeSettingsStorage : ISettingsStorage {
  std::vector<uint8_t> blob;
  bool present = false;
  int saveCalls = 0;

  size_t load(uint8_t *buf, size_t cap) override {
    if (!present || blob.size() > cap) {
      return 0;
    }
    std::memcpy(buf, blob.data(), blob.size());
    return blob.size();
  }

  bool save(const uint8_t *buf, size_t len) override {
    blob.assign(buf, buf + len);
    present = true;
    saveCalls++;
    return true;
  }
};
