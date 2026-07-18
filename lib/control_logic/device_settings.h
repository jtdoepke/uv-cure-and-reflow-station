// Controller-side device-settings store (design.md §4/§7/§24; Wave R2b of the §2 "CYD is a UI
// remote" split, 2026-07-17). Owns the single persisted settings blob (a versioned header + a
// nanopb-encoded oven_Settings) over an ISettingsStorage adapter — the controller analogue of the
// CYD's old lib/app_logic/settings_store.h, which the CYD sheds in R3.
//
// Two things make this the CONTROLLER's store, not just a moved copy:
//   1. It re-clamps the per-mode temp caps to the controller's OWN reviewed hard-max
//      (oven_safety.h) on every load AND save — so this resolves §4's defense-in-depth open: the
//      user caps now live on the safety MCU and are bounded there, not only CYD-side. A blob
//      hand-forged with uv_max_cap = 9999 can never take effect.
//   2. The display-only fields (brightness/sleep) are persisted verbatim and handed back — the
//      controller has no screen, so it does not police their ranges; the CYD's editor does (its
//      NumericFieldConfig bounds, §24). That is the intended two-layer split.
//
// Pure C++ over nanopb — host-tested under native_control against a FakeSettingsStorage.
#pragma once

#include <cstddef>
#include <cstdint>

#include "ISettingsStorage.h"
#include "codec.h"
#include "oven.pb.h"
#include "oven_safety.h"

namespace control {

// Factory defaults (design.md §4/§24): units C, auto-brightness on, UV cap 100, reflow cap 250.
// A fresh controller (no blob yet) hands these to the CYD on the first SettingsGet.
inline oven_Settings defaultSettings() {
  oven_Settings s = oven_Settings_init_zero;
  s.units = oven_TempUnits_TEMP_UNITS_CELSIUS;
  s.auto_brightness = true;
  s.advanced_unlocked = false;
  s.brightness_bias = 0;
  s.screen_brightness_pct = 80;
  s.idle_timeout_min = 2;
  s.uv_max_cap = 100;
  s.reflow_max_cap = 250;
  return s;
}

class SettingsStore {
public:
  explicit SettingsStore(ISettingsStorage &storage) : storage_(storage) { s_ = defaultSettings(); }

  // Load the persisted blob (or defaults on a blank/short/corrupt/version-mismatched one), then
  // clamp the caps to the current hard-max — the §4 boot clamp, so a firmware update that lowers a
  // hard-max can never leave a stale higher cap in effect.
  void load() {
    uint8_t buf[kBlobCap];
    const size_t n = storage_.load(buf, sizeof(buf));
    oven_Settings decoded = oven_Settings_init_zero;
    if (checkHeader(buf, n) &&
        protocol::decode(oven_Settings_fields, &decoded, buf + kHeaderLen, n - kHeaderLen)) {
      s_ = decoded;
    } else {
      s_ = defaultSettings();
    }
    clampCaps();
  }

  // Persist `in` (clamping caps first). Returns false on write failure.
  bool save(const oven_Settings &in) {
    s_ = in;
    clampCaps();
    uint8_t buf[kBlobCap];
    writeHeader(buf);
    size_t body = 0;
    if (!protocol::encode(oven_Settings_fields, &s_, buf + kHeaderLen, sizeof(buf) - kHeaderLen,
                          body)) {
      return false;
    }
    return storage_.save(buf, kHeaderLen + body);
  }

  const oven_Settings &get() const { return s_; }

  // Blob capacity a caller must provide (header + the largest nanopb Settings).
  static constexpr size_t kBlobCap = 6 + oven_Settings_size;

private:
  static constexpr uint32_t kMagic = 0x53455432; // "SET2" — nanopb format
  static constexpr uint16_t kVersion = 1;
  static constexpr size_t kHeaderLen = 6;

  static void writeHeader(uint8_t *buf) {
    buf[0] = static_cast<uint8_t>(kMagic & 0xFF);
    buf[1] = static_cast<uint8_t>((kMagic >> 8) & 0xFF);
    buf[2] = static_cast<uint8_t>((kMagic >> 16) & 0xFF);
    buf[3] = static_cast<uint8_t>((kMagic >> 24) & 0xFF);
    buf[4] = static_cast<uint8_t>(kVersion & 0xFF);
    buf[5] = static_cast<uint8_t>((kVersion >> 8) & 0xFF);
  }

  static bool checkHeader(const uint8_t *buf, size_t len) {
    if (len < kHeaderLen) {
      return false;
    }
    const uint32_t magic = static_cast<uint32_t>(buf[0]) | (static_cast<uint32_t>(buf[1]) << 8) |
                           (static_cast<uint32_t>(buf[2]) << 16) |
                           (static_cast<uint32_t>(buf[3]) << 24);
    const uint16_t version = static_cast<uint16_t>(buf[4]) | (static_cast<uint16_t>(buf[5]) << 8);
    return magic == kMagic && version == kVersion;
  }

  // Clamp each per-mode cap into (0, hard-max]; an absent/nonsensical value falls back to its
  // default. The ceiling is the safety-relevant bound (§4 layer 1).
  void clampCaps() {
    s_.uv_max_cap = clampCap(s_.uv_max_cap, defaultSettings().uv_max_cap,
                             static_cast<int32_t>(oven_safety::CURE_HARD_MAX_C));
    s_.reflow_max_cap = clampCap(s_.reflow_max_cap, defaultSettings().reflow_max_cap,
                                 static_cast<int32_t>(oven_safety::REFLOW_HARD_MAX_C));
  }

  static int32_t clampCap(int32_t v, int32_t dflt, int32_t hardMax) {
    if (v <= 0) {
      v = dflt;
    }
    return v > hardMax ? hardMax : v;
  }

  ISettingsStorage &storage_;
  oven_Settings s_{};
};

} // namespace control
