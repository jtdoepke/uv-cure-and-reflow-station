// MemSettingsStorage — an in-RAM ISettingsStorage (Wave R3b of the §2 "CYD is a UI remote" split).
// The CYD no longer persists settings — they live on the controller (§4/§7/§24) — so SettingsStore
// uses this as a RAM cache: fetched from the controller at connect, and pushed back on change.
// save() (called by SettingsStore::save() on each edit) fires on_saved so main.cpp queues a
// SettingsPut. Replaces the NVS adapter; keeps the settings screen + store unchanged.
//
// Pure C++ (no Arduino), but lives in src_cyd/ as firmware wiring beside its old NVS sibling.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "ISettingsStorage.h"

class MemSettingsStorage : public ISettingsStorage {
public:
  size_t load(uint8_t *buf, size_t cap) override {
    if (len_ == 0 || len_ > cap) {
      return 0; // nothing cached yet → SettingsStore falls back to defaults
    }
    std::memcpy(buf, blob_, len_);
    return len_;
  }

  bool save(const uint8_t *buf, size_t len) override {
    if (len > sizeof(blob_)) {
      return false;
    }
    std::memcpy(blob_, buf, len);
    len_ = len;
    if (on_saved_ != nullptr) {
      on_saved_(user_);
    }
    return true;
  }

  // Notified whenever the store writes (a settings edit) so main can push it to the controller.
  void setOnSaved(void (*cb)(void *user), void *user) {
    on_saved_ = cb;
    user_ = user;
  }

private:
  uint8_t blob_[128] = {};
  size_t len_ = 0;
  void (*on_saved_)(void *) = nullptr;
  void *user_ = nullptr;
};
