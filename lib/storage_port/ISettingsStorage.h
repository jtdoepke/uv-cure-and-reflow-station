// ISettingsStorage — the persist-a-settings-blob port (design.md §7, §24).
//
// REHOMED by Wave R2 (the §2 "CYD is a UI remote" split): the authoritative settings blob lives
// on the CONTROLLER now, behind src_control/littlefs_settings_storage. The CYD still constructs a
// SettingsStore over this port, but backed by an in-RAM adapter (src_cyd/mem_settings_storage) —
// a cache it syncs to the controller over the link (§9 SettingsGet/Put), not persistence.
//
// The store (control::SettingsStore / the CYD's SettingsStore) owns the typed fields +
// serialization + validation; this port is deliberately just "load/save one opaque blob", so the
// same interface backs a LittleFS file, an in-RAM cache, or a host fake driving the unit tests.
// Keeping it blob-shaped, not key-value, means the store decides the byte layout and the adapter
// stays dumb.
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
