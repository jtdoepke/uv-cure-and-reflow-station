// ISettingsStorage — the CYD's persist-a-settings-blob port (design.md §7, §24).
//
// SettingsStore (lib/app_logic) owns the typed fields + serialization + validation; this port
// is deliberately just "load/save one opaque blob", so the same interface backs whatever the
// firmware uses (NVS/Preferences today — src_cyd/nvs_settings_storage — or a LittleFS file
// later) and a host in-memory fake drives the store's unit tests. Keeping it blob-shaped, not
// key-value, means the store decides the byte layout and the adapter stays dumb.
//
// Keep this header free of <Arduino.h> so it stays native-compilable (mirrors the
// lib/control_port and lib/display_port idiom).
#pragma once

#include <cstddef>
#include <cstdint>

struct ISettingsStorage {
  virtual ~ISettingsStorage() = default;

  // Copy the persisted blob into `buf` (at most `cap` bytes) and return the number of bytes
  // read. Returns 0 when nothing is stored yet (first boot) or the blob doesn't fit `cap` —
  // the store treats either as "absent" and falls back to defaults.
  virtual size_t load(uint8_t *buf, size_t cap) = 0;

  // Persist `len` bytes from `buf`, replacing any previous blob. Returns false on write failure.
  virtual bool save(const uint8_t *buf, size_t len) = 0;
};
