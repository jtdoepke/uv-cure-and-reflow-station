// native_logic_cyd suite — phase_codec (design.md §9; Wave R3 of the §2 "CYD is a UI remote"
// split). The editor keeps working on the domain Phase[]; this codec bridges to the wire
// oven_Phase/oven_Profile. Pins the field mapping (esp. the RecipeMode<->oven_Mode value shift and
// the FanMode tri-state) and the profile round-trip.
#include <unity.h>

#include <cstring>

#include "oven.pb.h"
#include "phase.h"
#include "phase_codec.h"

void setUp(void) {}
void tearDown(void) {}

void test_fan_mode_maps(void) {
  TEST_ASSERT_EQUAL_INT(oven_FanMode_FAN_MODE_AUTO, phase_codec::fanToWire(FanMode::Auto));
  TEST_ASSERT_EQUAL_INT(oven_FanMode_FAN_MODE_ON, phase_codec::fanToWire(FanMode::On));
  TEST_ASSERT_EQUAL_INT(oven_FanMode_FAN_MODE_OFF, phase_codec::fanToWire(FanMode::Off));
  TEST_ASSERT_TRUE(FanMode::Auto == phase_codec::fanFromWire(oven_FanMode_FAN_MODE_AUTO));
  TEST_ASSERT_TRUE(FanMode::On == phase_codec::fanFromWire(oven_FanMode_FAN_MODE_ON));
  TEST_ASSERT_TRUE(FanMode::Off == phase_codec::fanFromWire(oven_FanMode_FAN_MODE_OFF));
}

// The load-bearing one: domain Cure=0/Reflow=1 vs wire CURE=1/REFLOW=2.
void test_mode_maps(void) {
  TEST_ASSERT_EQUAL_INT(oven_Mode_MODE_CURE, phase_codec::modeToWire(RecipeMode::Cure));
  TEST_ASSERT_EQUAL_INT(oven_Mode_MODE_REFLOW, phase_codec::modeToWire(RecipeMode::Reflow));
  TEST_ASSERT_TRUE(RecipeMode::Cure == phase_codec::modeFromWire(oven_Mode_MODE_CURE));
  TEST_ASSERT_TRUE(RecipeMode::Reflow == phase_codec::modeFromWire(oven_Mode_MODE_REFLOW));
}

void test_phase_roundtrip(void) {
  Phase p;
  std::strncpy(p.name, "Reflow", kPhaseNameCap - 1);
  p.targetC = 245.0F;
  p.rampSeconds = 35.0F;
  p.holdSeconds = 30.0F;
  p.exposurePerSurface = 12.5F;
  p.uv = true;
  p.motor = true;
  p.convFan = FanMode::On;

  Phase back = phase_codec::phaseFromWire(phase_codec::phaseToWire(p));
  TEST_ASSERT_EQUAL_STRING("Reflow", back.name);
  TEST_ASSERT_EQUAL_FLOAT(245.0F, back.targetC);
  TEST_ASSERT_EQUAL_FLOAT(35.0F, back.rampSeconds);
  TEST_ASSERT_EQUAL_FLOAT(30.0F, back.holdSeconds);
  TEST_ASSERT_EQUAL_FLOAT(12.5F, back.exposurePerSurface);
  TEST_ASSERT_TRUE(back.uv);
  TEST_ASSERT_TRUE(back.motor);
  TEST_ASSERT_TRUE(FanMode::On == back.convFan);
}

void test_profile_roundtrip(void) {
  Phase phases[3];
  std::strncpy(phases[0].name, "Preheat", kPhaseNameCap - 1);
  phases[0].targetC = 150.0F;
  phases[0].convFan = FanMode::Auto;
  std::strncpy(phases[1].name, "Soak", kPhaseNameCap - 1);
  phases[1].targetC = 180.0F;
  std::strncpy(phases[2].name, "Reflow", kPhaseNameCap - 1);
  phases[2].targetC = 245.0F;

  oven_Profile w = phase_codec::profileToWire("LF-245", RecipeMode::Reflow, false, phases, 3);
  TEST_ASSERT_EQUAL_STRING("LF-245", w.name);
  TEST_ASSERT_EQUAL_INT(oven_Mode_MODE_REFLOW, w.mode);
  TEST_ASSERT_FALSE(w.stock);
  TEST_ASSERT_EQUAL_UINT32(3, w.phases_count);

  Phase back[kMaxPhases];
  size_t n = phase_codec::phasesFromWire(w, back, kMaxPhases);
  TEST_ASSERT_EQUAL_UINT32(3, n);
  TEST_ASSERT_EQUAL_STRING("Preheat", back[0].name);
  TEST_ASSERT_EQUAL_FLOAT(245.0F, back[2].targetC);
}

// profileToWire clamps a Phase[] longer than the wire array bound.
void test_profile_clamps_count(void) {
  Phase phases[kMaxPhases + 8];
  for (auto &p : phases) {
    p.targetC = 100.0F;
  }
  oven_Profile w =
      phase_codec::profileToWire("big", RecipeMode::Cure, false, phases, kMaxPhases + 8);
  const size_t bound = sizeof(w.phases) / sizeof(w.phases[0]);
  TEST_ASSERT_EQUAL_UINT32(bound, w.phases_count);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_fan_mode_maps);
  RUN_TEST(test_mode_maps);
  RUN_TEST(test_phase_roundtrip);
  RUN_TEST(test_profile_roundtrip);
  RUN_TEST(test_profile_clamps_count);
  return UNITY_END();
}
