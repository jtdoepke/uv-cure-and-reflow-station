// native_logic_cyd suite — pure host tests of AutoBrightness behind the IAmbientLight/IBacklight
// ports. No LovyanGFX, no LVGL, no Arduino: fakes are injected and time is hand-advanced.
//
// Defaults under test (auto_brightness.h Config): updateIntervalMs=250, floor=48, ceil=255,
// rampStep=16, hysteresis=6, manualNominal=160, emaAlpha=0.25. Curve breakpoints (INVERTED for the
// CYD LDR: low raw = bright room = bright screen; high raw = dark = dim):
// {0,255} {80,150} {300,90} {800,55} {1400,48}.
#include <unity.h>

#include "auto_brightness.h"
#include "helpers/fake_ambient_light.h"
#include "helpers/fake_backlight.h"

void setUp(void) {}
void tearDown(void) {}

// Drive n updates at the 250 ms update cadence (first tick seeds + updates, each later tick is
// >= updateIntervalMs later so it updates too), enough to let the ramp settle.
static void settle(AutoBrightness &ab, int n = 24) {
  for (int i = 0; i < n; i++) {
    ab.tick(static_cast<uint32_t>(i) * 250U);
  }
}

void test_floor_respected_in_darkness(void) {
  FakeAmbientLight ldr;
  FakeBacklight bl;
  ldr.value = 2000; // near-blackout: high raw -> curve clamps to the 48 floor
  AutoBrightness ab(ldr, bl);
  settle(ab);
  TEST_ASSERT_EQUAL_UINT8(48, ab.level()); // clamped to the safety floor
  TEST_ASSERT_EQUAL_UINT8(48, bl.level);
}

void test_ceiling_respected_in_bright(void) {
  FakeAmbientLight ldr;
  FakeBacklight bl;
  ldr.value = 0; // bright room: raw ~0 -> curve wants full 255
  AutoBrightness ab(ldr, bl);
  settle(ab);
  TEST_ASSERT_EQUAL_UINT8(255, ab.level());
}

void test_single_tick_does_not_jump_full_range(void) {
  FakeAmbientLight ldr;
  FakeBacklight bl;
  ldr.value = 0; // bright -> wants 255, but one update may move only rampStep
  AutoBrightness ab(ldr, bl);
  ab.tick(0);
  TEST_ASSERT_EQUAL_UINT8(16, ab.level()); // one rampStep from black, not a jump to 255
  TEST_ASSERT_TRUE(ab.level() < 255);
}

void test_update_is_gated_to_the_sample_interval(void) {
  FakeAmbientLight ldr;
  FakeBacklight bl;
  ldr.value = 0;
  AutoBrightness ab(ldr, bl);
  ab.tick(0); // first update: one sample, one ramp step
  TEST_ASSERT_EQUAL_INT(1, ldr.reads);
  TEST_ASSERT_EQUAL_UINT8(16, ab.level());
  ab.tick(100); // < updateIntervalMs -> no re-sample, no ramp
  TEST_ASSERT_EQUAL_INT(1, ldr.reads);
  TEST_ASSERT_EQUAL_UINT8(16, ab.level());
  ab.tick(250); // interval elapsed -> update
  TEST_ASSERT_EQUAL_INT(2, ldr.reads);
  TEST_ASSERT_EQUAL_UINT8(32, ab.level());
}

void test_single_sample_spike_is_smoothed(void) {
  FakeAmbientLight ldr;
  FakeBacklight bl;
  ldr.value = 0; // bright room -> curve 255
  AutoBrightness ab(ldr, bl);
  settle(ab);
  TEST_ASSERT_EQUAL_UINT8(255, ab.level());

  // A shadow passes over: one dark sample (raw 1400 -> curve wants floor 48), then back to bright.
  ldr.value = 1400;
  ab.tick(24U * 250U);
  ldr.value = 0;
  // The EMA + ramp keep the backlight far from the 48 the raw spike alone would command.
  TEST_ASSERT_TRUE(ab.level() > 200);
  TEST_ASSERT_TRUE(ab.level() <= 255);
}

void test_sub_threshold_drift_is_held_by_hysteresis(void) {
  FakeAmbientLight ldr;
  FakeBacklight bl;
  ldr.value = 800; // curve breakpoint -> 55
  AutoBrightness ab(ldr, bl);
  settle(ab);
  TEST_ASSERT_EQUAL_UINT8(55, ab.level());

  // A tiny ambient change whose curve delta (<6 levels) sits under the hysteresis band.
  ldr.value = 850;
  for (int i = 0; i < 12; i++) {
    ab.tick((24U + static_cast<uint32_t>(i)) * 250U);
  }
  TEST_ASSERT_EQUAL_UINT8(55, ab.level()); // held, not chased
}

void test_sleep_ramps_backlight_off(void) {
  FakeAmbientLight ldr;
  FakeBacklight bl;
  ldr.value = 0; // bright -> settle at full 255
  AutoBrightness ab(ldr, bl);
  settle(ab);
  TEST_ASSERT_EQUAL_UINT8(255, ab.level());

  ab.setAwake(false);
  ab.tick(24U * 250U);
  TEST_ASSERT_EQUAL_UINT8(239, ab.level()); // eases off by one step, not an instant blackout
  for (int i = 1; i < 24; i++) {
    ab.tick((24U + static_cast<uint32_t>(i)) * 250U);
  }
  TEST_ASSERT_EQUAL_UINT8(0, ab.level()); // fully off (floor bypassed while asleep)

  // Waking resumes auto brightness at full level in ONE update — it does not ease back up from
  // black. Asymmetric on purpose: see update(). (This line used to expect 16, i.e. a single
  // rampStep, when waking faded in over ~4 s.)
  ab.setAwake(true);
  ab.tick(60U * 250U);
  TEST_ASSERT_EQUAL_UINT8(255, ab.level());
}

void test_bias_shifts_target_but_floor_always_wins(void) {
  // Positive bias lifts even a floored dark reading well up off the floor (the additive-trim fix:
  // a multiplicative bias on a below-floor curve value stayed stuck at the floor -> looked dead).
  {
    FakeAmbientLight ldr;
    FakeBacklight bl;
    ldr.value = 1400; // dark -> curve 48 (floor) with no bias
    AutoBrightness ab(ldr, bl);
    ab.setBias(40);
    settle(ab);
    TEST_ASSERT_EQUAL_UINT8(150, ab.level()); // 48 + 40% of 255 (102)
  }
  // Negative bias lowers a bright level.
  {
    FakeAmbientLight ldr;
    FakeBacklight bl;
    ldr.value = 0; // bright -> base curve 255
    AutoBrightness ab(ldr, bl);
    ab.setBias(-40);
    settle(ab);
    TEST_ASSERT_EQUAL_UINT8(153, ab.level()); // 255 - 102
  }
  // ...but the safety floor is never defeated, even at max negative bias in the dark.
  {
    FakeAmbientLight ldr;
    FakeBacklight bl;
    ldr.value = 1400; // dark -> base curve 48
    AutoBrightness ab(ldr, bl);
    ab.setBias(-40);
    settle(ab);
    TEST_ASSERT_EQUAL_UINT8(48, ab.level()); // 48 - 102 -> floor wins
  }
}

void test_disabled_holds_manual_nominal_with_bias(void) {
  FakeAmbientLight ldr;
  FakeBacklight bl;
  ldr.value = 1400; // any ambient must be ignored when auto is off
  AutoBrightness ab(ldr, bl);
  ab.setEnabled(false);
  settle(ab);
  TEST_ASSERT_EQUAL_UINT8(160, ab.level()); // manualNominal, ambient ignored
  TEST_ASSERT_EQUAL_INT(0, ldr.reads);      // LDR not sampled while auto is off
}

// setManualPercent is the whole brightness control on a board with no light sensor (§18): the
// stored Screen brightness is pushed in as the manual level every loop, so it must land exactly,
// not approximately, and must keep working when re-set live (the editor's preview re-pushes it on
// every tick while the user dials).
void test_manual_percent_sets_the_level_exactly(void) {
  FakeAmbientLight ldr;
  FakeBacklight bl;
  AutoBrightness ab(ldr, bl);
  ab.setEnabled(false);
  ab.setBias(0);

  ab.setManualPercent(100);
  settle(ab);
  TEST_ASSERT_EQUAL_UINT8(255, ab.level());

  ab.setManualPercent(60); // 60% of 255 = 153
  settle(ab);
  TEST_ASSERT_EQUAL_UINT8(153, ab.level());

  TEST_ASSERT_EQUAL_INT(0, ldr.reads); // still never samples a sensor that is not there
}

// The safety floor is not this field's to escape: below it, the level clamps. The settings floor
// (SCREEN_BRIGHTNESS_MIN_PCT = 20%) is pitched above it precisely so the user never meets this
// clamp — but if someone lowers that constant, this is the behaviour they would be shipping.
void test_manual_percent_still_cannot_defeat_the_safety_floor(void) {
  FakeAmbientLight ldr;
  FakeBacklight bl;
  AutoBrightness ab(ldr, bl);
  ab.setEnabled(false);
  ab.setBias(0);

  ab.setManualPercent(20); // the settings minimum: 51 > floor 48, so it lands untouched
  settle(ab);
  TEST_ASSERT_EQUAL_UINT8(51, ab.level());

  ab.setManualPercent(0); // below the floor -> clamped, NOT black
  settle(ab);
  TEST_ASSERT_EQUAL_UINT8(48, ab.level());
}

// Waking must land at full level in ONE update, not ease in over ~1 s of rampStep=16 increments
// (255/16 = 16 updates x 250 ms). The fade-OUT stays — the two directions are not symmetric.
void test_wake_snaps_to_full_but_sleep_still_fades(void) {
  FakeAmbientLight ldr;
  FakeBacklight bl;
  ldr.value = 0; // bright room -> target 255
  AutoBrightness ab(ldr, bl);
  settle(ab);
  TEST_ASSERT_EQUAL_UINT8(255, ab.level());

  // Sleep: still a smooth ramp down, and one update must NOT reach 0.
  ab.setAwake(false);
  ab.tick(6000);
  TEST_ASSERT_TRUE(ab.level() > 0);   // eased, not snapped off
  TEST_ASSERT_TRUE(ab.level() < 255); // but moving
  for (int i = 1; i <= 24; i++) {
    ab.tick(6000 + static_cast<uint32_t>(i) * 250U);
  }
  TEST_ASSERT_EQUAL_UINT8(0, ab.level()); // ...and it gets all the way there

  // Wake: one update, straight to the target. This is the assertion that fails on a ramp.
  ab.setAwake(true);
  ab.tick(20000);
  TEST_ASSERT_EQUAL_UINT8(255, ab.level());
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_floor_respected_in_darkness);
  RUN_TEST(test_ceiling_respected_in_bright);
  RUN_TEST(test_single_tick_does_not_jump_full_range);
  RUN_TEST(test_update_is_gated_to_the_sample_interval);
  RUN_TEST(test_single_sample_spike_is_smoothed);
  RUN_TEST(test_sub_threshold_drift_is_held_by_hysteresis);
  RUN_TEST(test_sleep_ramps_backlight_off);
  RUN_TEST(test_wake_snaps_to_full_but_sleep_still_fades);
  RUN_TEST(test_bias_shifts_target_but_floor_always_wins);
  RUN_TEST(test_disabled_holds_manual_nominal_with_bias);
  RUN_TEST(test_manual_percent_sets_the_level_exactly);
  RUN_TEST(test_manual_percent_still_cannot_defeat_the_safety_floor);
  return UNITY_END();
}
