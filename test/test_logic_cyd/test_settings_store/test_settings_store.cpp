// native_logic_cyd suite — pure host tests of SettingsStore (backlog B5), the typed device-
// settings model + persistence behind the ISettingsStorage port. No LVGL, no Arduino: a
// FakeSettingsStorage stands in for NVS. Covers defaults on a blank device, save/load round-trip,
// setter clamping, the boot clamp-to-hard-max (the safety-relevant bit, §4/§7/§24), and graceful
// fallback on a short / bad-magic / wrong-version blob.
#include <unity.h>

#include <cstdint>
#include <cstring>

#include "helpers/fake_settings_storage.h"
#include "settings_defaults.h"
#include "settings_store.h"

void setUp(void) {}
void tearDown(void) {}

// --- Defaults ---

void test_defaults_on_blank_device(void) {
  FakeSettingsStorage fs; // present = false
  SettingsStore store(fs);
  store.load();
  TEST_ASSERT_EQUAL_INT(static_cast<int>(TempUnits::Celsius), static_cast<int>(store.units()));
  TEST_ASSERT_TRUE(store.autoBrightness());
  TEST_ASSERT_FALSE(store.advancedUnlocked());
  TEST_ASSERT_EQUAL_INT32(settings_defaults::BRIGHTNESS_BIAS_DEFAULT, store.brightnessBias());
  TEST_ASSERT_EQUAL_INT32(settings_defaults::IDLE_TIMEOUT_DEFAULT_MIN, store.idleTimeoutMin());
  TEST_ASSERT_EQUAL_INT32(settings_defaults::UV_CAP_DEFAULT, store.uvMaxCap());
  TEST_ASSERT_EQUAL_INT32(settings_defaults::REFLOW_CAP_DEFAULT, store.reflowMaxCap());
}

// --- Round-trip ---

void test_save_load_round_trip(void) {
  FakeSettingsStorage fs;
  {
    SettingsStore store(fs);
    store.load(); // defaults
    store.setUnits(TempUnits::Fahrenheit);
    store.setAutoBrightness(false);
    store.setAdvancedUnlocked(true);
    store.setBrightnessBias(15);
    store.setIdleTimeoutMin(7);
    store.setUvMaxCap(110);
    store.setReflowMaxCap(240);
    TEST_ASSERT_TRUE(store.save());
    TEST_ASSERT_EQUAL_INT(1, fs.saveCalls);
  }
  // Fresh store over the same (now-populated) storage reads the saved values back.
  SettingsStore reloaded(fs);
  reloaded.load();
  TEST_ASSERT_EQUAL_INT(static_cast<int>(TempUnits::Fahrenheit),
                        static_cast<int>(reloaded.units()));
  TEST_ASSERT_FALSE(reloaded.autoBrightness());
  TEST_ASSERT_TRUE(reloaded.advancedUnlocked());
  TEST_ASSERT_EQUAL_INT32(15, reloaded.brightnessBias());
  TEST_ASSERT_EQUAL_INT32(7, reloaded.idleTimeoutMin());
  TEST_ASSERT_EQUAL_INT32(110, reloaded.uvMaxCap());
  TEST_ASSERT_EQUAL_INT32(240, reloaded.reflowMaxCap());
}

// --- Setter clamping ---

void test_setters_clamp_to_field_bounds(void) {
  FakeSettingsStorage fs;
  SettingsStore store(fs);
  store.load();
  // Idle timeout config is 1..10.
  store.setIdleTimeoutMin(999);
  TEST_ASSERT_EQUAL_INT32(10, store.idleTimeoutMin());
  store.setIdleTimeoutMin(-4);
  TEST_ASSERT_EQUAL_INT32(1, store.idleTimeoutMin());
  // UV cap can never be set above the UV hard-max, even directly.
  store.setUvMaxCap(settings_defaults::UV_HARD_MAX + 50);
  TEST_ASSERT_EQUAL_INT32(settings_defaults::UV_HARD_MAX, store.uvMaxCap());
  // Reflow cap ceiling is the reflow hard-max (300).
  store.setReflowMaxCap(400);
  TEST_ASSERT_EQUAL_INT32(settings_defaults::REFLOW_HARD_MAX, store.reflowMaxCap());
  // Brightness bias is a signed nudge, clamped both ways.
  store.setBrightnessBias(1000);
  TEST_ASSERT_EQUAL_INT32(40, store.brightnessBias());
  store.setBrightnessBias(-1000);
  TEST_ASSERT_EQUAL_INT32(-40, store.brightnessBias());
}

// --- Boot clamp-to-hard-max (the safety-relevant behaviour) ---
//
// Simulate a blob persisted by an older firmware whose UV/reflow ceilings were higher: the stored
// caps sit above the current hard-max. load() must clamp them down so a downgraded ceiling can't
// leave a stale higher cap in effect (§4/§7/§24). We forge the blob by saving from one store, then
// overwriting the two cap fields in the raw bytes to out-of-range values.
void test_load_clamps_caps_to_current_hard_max(void) {
  FakeSettingsStorage fs;
  {
    SettingsStore seed(fs);
    seed.load();
    seed.save(); // lay down a valid, current-version blob we can mutate in place
  }
  // The two int32 caps are the last two fields of PersistedBlob; overwrite them past the ceilings.
  const int32_t forged_uv = settings_defaults::UV_HARD_MAX + 80;
  const int32_t forged_reflow = settings_defaults::REFLOW_HARD_MAX + 80;
  const size_t n = fs.blob.size();
  std::memcpy(fs.blob.data() + n - 2 * sizeof(int32_t), &forged_uv, sizeof(int32_t));
  std::memcpy(fs.blob.data() + n - 1 * sizeof(int32_t), &forged_reflow, sizeof(int32_t));

  SettingsStore store(fs);
  store.load();
  TEST_ASSERT_EQUAL_INT32(settings_defaults::UV_HARD_MAX, store.uvMaxCap());
  TEST_ASSERT_EQUAL_INT32(settings_defaults::REFLOW_HARD_MAX, store.reflowMaxCap());
}

// --- Corrupt / foreign / wrong-version blobs fall back to defaults ---

void test_bad_magic_falls_back_to_defaults(void) {
  FakeSettingsStorage fs;
  {
    SettingsStore seed(fs);
    seed.load();
    seed.setUvMaxCap(115);
    seed.save();
  }
  // Corrupt the magic (first 4 bytes).
  uint32_t junk = 0xDEADBEEF;
  std::memcpy(fs.blob.data(), &junk, sizeof(junk));

  SettingsStore store(fs);
  store.load();
  TEST_ASSERT_EQUAL_INT32(settings_defaults::UV_CAP_DEFAULT, store.uvMaxCap());
}

void test_short_blob_falls_back_to_defaults(void) {
  FakeSettingsStorage fs;
  fs.present = true;
  fs.blob.assign(3, 0xFF); // too short to be a PersistedBlob
  SettingsStore store(fs);
  store.load();
  TEST_ASSERT_EQUAL_INT32(settings_defaults::REFLOW_CAP_DEFAULT, store.reflowMaxCap());
}

// --- Restore defaults ---

void test_restore_defaults(void) {
  FakeSettingsStorage fs;
  SettingsStore store(fs);
  store.load();
  store.setUnits(TempUnits::Fahrenheit);
  store.setUvMaxCap(118);
  store.setAdvancedUnlocked(true);
  store.restoreDefaults();
  TEST_ASSERT_EQUAL_INT(static_cast<int>(TempUnits::Celsius), static_cast<int>(store.units()));
  TEST_ASSERT_EQUAL_INT32(settings_defaults::UV_CAP_DEFAULT, store.uvMaxCap());
  TEST_ASSERT_FALSE(store.advancedUnlocked());
}

// --- The per-field configs drive the >20-step editor routing (§24) ---

void test_field_configs_route_to_expected_editor(void) {
  TEST_ASSERT_TRUE(SettingsStore::idleTimeoutConfig().usesStepper());
  TEST_ASSERT_TRUE(SettingsStore::brightnessBiasConfig().usesStepper());
  TEST_ASSERT_FALSE(SettingsStore::uvCapConfig().usesStepper());     // wide range -> keypad
  TEST_ASSERT_FALSE(SettingsStore::reflowCapConfig().usesStepper()); // wide range -> keypad
  // The cap editors' ceiling is the hard-max, so the keypad refuses anything above it by design.
  TEST_ASSERT_EQUAL_INT32(settings_defaults::UV_HARD_MAX, SettingsStore::uvCapConfig().max);
  TEST_ASSERT_EQUAL_INT32(settings_defaults::REFLOW_HARD_MAX, SettingsStore::reflowCapConfig().max);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_defaults_on_blank_device);
  RUN_TEST(test_save_load_round_trip);
  RUN_TEST(test_setters_clamp_to_field_bounds);
  RUN_TEST(test_load_clamps_caps_to_current_hard_max);
  RUN_TEST(test_bad_magic_falls_back_to_defaults);
  RUN_TEST(test_short_blob_falls_back_to_defaults);
  RUN_TEST(test_restore_defaults);
  RUN_TEST(test_field_configs_route_to_expected_editor);
  return UNITY_END();
}
