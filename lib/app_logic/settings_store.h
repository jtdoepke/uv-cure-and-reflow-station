// SettingsStore — the typed device-settings model + persistence (design.md §4, §7, §24; backlog
// B5). Owns the CYD's user preferences (temperature units, per-mode max-temp caps, sleep and
// brightness constants, the Advanced unlock), serializes them to a single versioned blob through
// the ISettingsStorage port, and — critically — re-clamps every value into its current bounds on
// load, so a firmware update that lowers a hard-max can never leave a stale higher temp cap in
// effect ("clamped to the current hard-max at boot", §4/§7/§24).
//
// Validation is baked in: setters and load() both route through the same per-field
// NumericFieldConfig (numeric_field.h) that the shared numeric editors use, so the store and the
// UI can never disagree about a field's bounds. The static *Config() factories are the single
// source of those bounds — the Settings panels (§24) build their editors from them.
//
// Pure C++: no LVGL, no Arduino, host-tested under native_logic_cyd against a FakeSettingsStorage.
// The real NVS/Preferences adapter is thin firmware glue in src_cyd/ (the "testable logic lives in
// lib/" guardrail).
#pragma once

#include <cstdint>
#include <cstring>

#include "ISettingsStorage.h"
#include "numeric_field.h"
#include "settings_defaults.h"

// Temperature display units (§24 "Display & units"). Stored so a °C/°F choice survives a reboot.
enum class TempUnits : uint8_t { Celsius = 0, Fahrenheit = 1 };

// The plain-old-data settings snapshot. Default-constructed = the firmware factory defaults
// (design.md:2251): units °C, UV cap 100 °C, reflow cap 250 °C, idle ~2 min, auto-brightness on.
struct Settings {
  TempUnits units = TempUnits::Celsius;
  bool autoBrightness = true;
  bool advancedUnlocked = false;
  int32_t brightnessBias = settings_defaults::BRIGHTNESS_BIAS_DEFAULT;
  int32_t screenBrightnessPct = settings_defaults::SCREEN_BRIGHTNESS_DEFAULT_PCT;
  int32_t idleTimeoutMin = settings_defaults::IDLE_TIMEOUT_DEFAULT_MIN;
  int32_t uvMaxCap = settings_defaults::UV_CAP_DEFAULT;
  int32_t reflowMaxCap = settings_defaults::REFLOW_CAP_DEFAULT;
};

class SettingsStore {
public:
  explicit SettingsStore(ISettingsStorage &storage) : storage_(storage) {}

  // Load persisted settings, or fall back to defaults on a blank/short/corrupt/version-mismatched
  // blob. Always ends by clamping every field into its current NumericFieldConfig bounds, so caps
  // are tightened to the current hard-max even when the stored blob was valid (the boot clamp).
  void load() {
    uint8_t buf[sizeof(PersistedBlob)];
    size_t n = storage_.load(buf, sizeof(buf));
    PersistedBlob b{};
    if (n == sizeof(PersistedBlob)) {
      std::memcpy(&b, buf, sizeof(b));
    }
    if (n == sizeof(PersistedBlob) && b.magic == kMagic && b.version == kVersion) {
      s_.units = b.units == static_cast<uint16_t>(TempUnits::Fahrenheit) ? TempUnits::Fahrenheit
                                                                         : TempUnits::Celsius;
      s_.autoBrightness = b.autoBrightness != 0;
      s_.advancedUnlocked = b.advancedUnlocked != 0;
      s_.brightnessBias = b.brightnessBias;
      s_.screenBrightnessPct = b.screenBrightnessPct;
      s_.idleTimeoutMin = b.idleTimeoutMin;
      s_.uvMaxCap = b.uvMaxCap;
      s_.reflowMaxCap = b.reflowMaxCap;
    } else {
      s_ = Settings{}; // absent / short / bad magic / wrong version -> factory defaults
    }
    clampAll();
  }

  // Serialize the current settings and hand the blob to the storage port. Returns false on write
  // failure. The UI calls this on a panel's Save (or right after an immediate toggle), §24.
  bool save() {
    PersistedBlob b{};
    b.magic = kMagic;
    b.version = kVersion;
    b.units = static_cast<uint16_t>(s_.units);
    b.autoBrightness = s_.autoBrightness ? 1 : 0;
    b.advancedUnlocked = s_.advancedUnlocked ? 1 : 0;
    b.brightnessBias = s_.brightnessBias;
    b.screenBrightnessPct = s_.screenBrightnessPct;
    b.idleTimeoutMin = s_.idleTimeoutMin;
    b.uvMaxCap = s_.uvMaxCap;
    b.reflowMaxCap = s_.reflowMaxCap;
    uint8_t buf[sizeof(PersistedBlob)];
    std::memcpy(buf, &b, sizeof(b));
    return storage_.save(buf, sizeof(buf));
  }

  // Reset every field to the firmware factory defaults ("Restore defaults", §24). Does not
  // persist on its own — the caller saves.
  void restoreDefaults() { s_ = Settings{}; }

  // --- Getters ---
  TempUnits units() const { return s_.units; }
  bool autoBrightness() const { return s_.autoBrightness; }
  bool advancedUnlocked() const { return s_.advancedUnlocked; }
  int32_t brightnessBias() const { return s_.brightnessBias; }
  int32_t screenBrightnessPct() const { return s_.screenBrightnessPct; }
  int32_t idleTimeoutMin() const { return s_.idleTimeoutMin; }
  int32_t uvMaxCap() const { return s_.uvMaxCap; }
  int32_t reflowMaxCap() const { return s_.reflowMaxCap; }
  const Settings &values() const { return s_; }

  // --- Setters (numeric ones clamp through their field config; the editors already constrain, so
  // this is the belt-and-braces guard). None persist — the caller saves. ---
  void setUnits(TempUnits u) { s_.units = u; }
  void setAutoBrightness(bool v) { s_.autoBrightness = v; }
  void setAdvancedUnlocked(bool v) { s_.advancedUnlocked = v; }
  void setBrightnessBias(int32_t v) { s_.brightnessBias = brightnessBiasConfig().clamp(v); }
  void setScreenBrightnessPct(int32_t v) {
    s_.screenBrightnessPct = screenBrightnessConfig().clamp(v);
  }
  void setIdleTimeoutMin(int32_t v) { s_.idleTimeoutMin = idleTimeoutConfig().clamp(v); }
  void setUvMaxCap(int32_t v) { s_.uvMaxCap = uvCapConfig().clamp(v); }
  void setReflowMaxCap(int32_t v) { s_.reflowMaxCap = reflowCapConfig().clamp(v); }

  // --- Per-field configs: the single source of bounds/default/units/caution the settings editors
  // build from. The >20-step rule (numeric_field.h) then picks stepper vs keypad automatically:
  // idle-timeout & brightness-bias stay steppers; the temp caps (max = hard-max) go to the keypad.
  static NumericFieldConfig idleTimeoutConfig() {
    return NumericFieldConfig{1,     10,     1, settings_defaults::IDLE_TIMEOUT_DEFAULT_MIN,
                              "min", nullptr};
  }
  static NumericFieldConfig brightnessBiasConfig() {
    return NumericFieldConfig{-40, 40, 5, settings_defaults::BRIGHTNESS_BIAS_DEFAULT, "%", nullptr};
  }
  // Absolute screen brightness — the no-sensor board's row instead of the bias (settings_defaults.h
  // on why, and on why the floor may not go lower). The clamp here is what makes the floor real for
  // a hand-written blob, not just for the editor.
  static NumericFieldConfig screenBrightnessConfig() {
    return NumericFieldConfig{settings_defaults::SCREEN_BRIGHTNESS_MIN_PCT,
                              settings_defaults::SCREEN_BRIGHTNESS_MAX_PCT,
                              settings_defaults::SCREEN_BRIGHTNESS_STEP_PCT,
                              settings_defaults::SCREEN_BRIGHTNESS_DEFAULT_PCT,
                              "%",
                              nullptr};
  }
  static NumericFieldConfig uvCapConfig() {
    return NumericFieldConfig{settings_defaults::TEMP_CAP_MIN,
                              settings_defaults::UV_HARD_MAX,
                              1,
                              settings_defaults::UV_CAP_DEFAULT,
                              "°C",
                              settings_defaults::UV_CAP_CAUTION};
  }
  static NumericFieldConfig reflowCapConfig() {
    return NumericFieldConfig{settings_defaults::TEMP_CAP_MIN,
                              settings_defaults::REFLOW_HARD_MAX,
                              1,
                              settings_defaults::REFLOW_CAP_DEFAULT,
                              "°C",
                              settings_defaults::REFLOW_CAP_CAUTION};
  }

private:
  // Fixed-layout on-flash representation. Explicit padding + magic/version make a layout change a
  // deliberate version bump (an old blob then fails the version check and falls back to defaults).
  struct PersistedBlob {
    uint32_t magic;
    uint16_t version;
    uint16_t units;
    uint8_t autoBrightness;
    uint8_t advancedUnlocked;
    uint8_t pad_[2];
    int32_t brightnessBias;
    int32_t screenBrightnessPct;
    int32_t idleTimeoutMin;
    int32_t uvMaxCap;
    int32_t reflowMaxCap;
  };

  static constexpr uint32_t kMagic = 0x53455431; // "SET1"
  // v2 added screenBrightnessPct. Bumped rather than squeezed into pad_, per this struct's own
  // contract: a layout change is a deliberate version bump, and a v1 blob then fails the check and
  // falls back to factory defaults. That costs a one-time reset of saved settings on the first boot
  // after this update — the honest price of not reading an old blob's padding as a brightness.
  static constexpr uint16_t kVersion = 2;

  // Clamp every field into its current config bounds — the boot clamp-to-hard-max plus a defensive
  // re-clamp of the nudge fields.
  void clampAll() {
    s_.brightnessBias = brightnessBiasConfig().clamp(s_.brightnessBias);
    s_.screenBrightnessPct = screenBrightnessConfig().clamp(s_.screenBrightnessPct);
    s_.idleTimeoutMin = idleTimeoutConfig().clamp(s_.idleTimeoutMin);
    s_.uvMaxCap = uvCapConfig().clamp(s_.uvMaxCap);
    s_.reflowMaxCap = reflowCapConfig().clamp(s_.reflowMaxCap);
  }

  ISettingsStorage &storage_;
  Settings s_{};
};
