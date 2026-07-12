// native_control suite — heater time-proportioning (design.md §11).
//
// Drives HeaterActuator with a FakeClock + recording FakeHeaterSwitch: advances the
// clock in fixed steps across a proportioning window and counts samples where the
// switch is on to measure realized duty. Covers snapping, the per-window latch, the
// forceOff safety override, duty clamping, and millis() wraparound.
#include <unity.h>

#include "heater_actuator.h"
#include "helpers/fake_clock.h"
#include "helpers/fake_heater_switch.h"

// Tick every stepMs across exactly one windowMs, returning the number of samples the
// switch was on. With step 10 / window 1000 that's 100 samples, so the count reads as
// "percent on" directly (30 => ~30 % duty).
static int countOnOverWindow(HeaterActuator &act, FakeClock &clk, FakeHeaterSwitch &sw,
                             uint32_t stepMs, uint32_t windowMs) {
  int on = 0;
  for (uint32_t t = 0; t < windowMs; t += stepMs) {
    act.tick();
    if (sw.on) {
      on++;
    }
    clk.advance(stepMs);
  }
  return on;
}

void setUp(void) {}
void tearDown(void) {}

// duty 0.3 over a 1 s window -> on ~300 ms (~30 of 100 samples).
void test_duty_030_is_about_30_percent(void) {
  FakeClock clk;
  FakeHeaterSwitch sw;
  HeaterActuator act(sw, clk);
  act.setDuty(0.3F);
  TEST_ASSERT_INT_WITHIN(1, 30, countOnOverWindow(act, clk, sw, 10, 1000));
}

// duty 0.0 -> always off (switch never asserted).
void test_duty_zero_is_always_off(void) {
  FakeClock clk;
  FakeHeaterSwitch sw;
  HeaterActuator act(sw, clk);
  act.setDuty(0.0F);
  TEST_ASSERT_EQUAL_INT(0, countOnOverWindow(act, clk, sw, 10, 1000));
}

// duty 1.0 -> always on (every sample).
void test_duty_one_is_always_on(void) {
  FakeClock clk;
  FakeHeaterSwitch sw;
  HeaterActuator act(sw, clk);
  act.setDuty(1.0F);
  TEST_ASSERT_EQUAL_INT(100, countOnOverWindow(act, clk, sw, 10, 1000));
}

// A tiny duty below minOnMs snaps to full off (no sliver pulses).
void test_tiny_duty_snaps_off(void) {
  FakeClock clk;
  FakeHeaterSwitch sw;
  HeaterActuator act(sw, clk); // minOnMs = 50; 0.01*1000 = 10 ms < 50
  act.setDuty(0.01F);
  TEST_ASSERT_EQUAL_INT(0, countOnOverWindow(act, clk, sw, 10, 1000));
  TEST_ASSERT_EQUAL_INT(0, sw.transitions); // never even toggled
}

// A near-full duty above (window - minOffMs) snaps to full on.
void test_near_full_duty_snaps_on(void) {
  FakeClock clk;
  FakeHeaterSwitch sw;
  HeaterActuator act(sw, clk); // minOffMs = 50; 0.99*1000 = 990 ms > 950
  act.setDuty(0.99F);
  TEST_ASSERT_EQUAL_INT(100, countOnOverWindow(act, clk, sw, 10, 1000));
}

// A mid-window setDuty() does not disturb the current window's on-time; it takes
// effect only at the next window latch.
void test_duty_latched_per_window(void) {
  FakeClock clk;
  FakeHeaterSwitch sw;
  HeaterActuator act(sw, clk);
  act.setDuty(0.3F);
  int win1 = 0;
  for (uint32_t t = 0; t < 1000; t += 10) {
    act.tick();
    if (t == 500) {
      act.setDuty(0.9F); // change mid-window, after this tick's sample
    }
    if (sw.on) {
      win1++;
    }
    clk.advance(10);
  }
  TEST_ASSERT_INT_WITHIN(1, 30, win1); // still the latched 0.3, not 0.9

  int win2 = countOnOverWindow(act, clk, sw, 10, 1000);
  TEST_ASSERT_INT_WITHIN(1, 90, win2); // next window reflects 0.9
}

// forceOff() turns the switch off in the same tick and holds it off, without waiting
// for the window boundary.
void test_force_off_is_immediate_and_sticky(void) {
  FakeClock clk;
  FakeHeaterSwitch sw;
  HeaterActuator act(sw, clk);
  act.setDuty(1.0F);
  act.tick(); // t=0: latched full-on, drives on
  TEST_ASSERT_TRUE(sw.on);
  clk.advance(100);
  act.tick();
  TEST_ASSERT_TRUE(sw.on);

  act.forceOff();
  TEST_ASSERT_FALSE(sw.on); // immediate, mid-window

  // Stays off through the rest of this window and into the next.
  clk.advance(100);
  act.tick();
  TEST_ASSERT_FALSE(sw.on);
  clk.advance(1000);
  act.tick();
  TEST_ASSERT_FALSE(sw.on);
}

// setDuty() clamps out-of-range commands: >1 behaves as full on, <0 as full off.
void test_duty_clamped(void) {
  {
    FakeClock clk;
    FakeHeaterSwitch sw;
    HeaterActuator act(sw, clk);
    act.setDuty(1.5F);
    TEST_ASSERT_EQUAL_INT(100, countOnOverWindow(act, clk, sw, 10, 1000));
  }
  {
    FakeClock clk;
    FakeHeaterSwitch sw;
    HeaterActuator act(sw, clk);
    act.setDuty(-0.2F);
    TEST_ASSERT_EQUAL_INT(0, countOnOverWindow(act, clk, sw, 10, 1000));
  }
}

// Window timing is wrap-safe: starting a window just before millis() rolls over 2^32
// still yields the correct on-time across the boundary.
void test_wraparound_window(void) {
  FakeClock clk;
  FakeHeaterSwitch sw;
  HeaterActuator act(sw, clk);
  clk.now = 0xFFFFFF00U; // 256 ms before wrap; window spans the rollover
  act.setDuty(0.3F);
  int on = 0;
  for (uint32_t i = 0; i < 100; i++) {
    act.tick();
    if (sw.on) {
      on++;
    }
    clk.advance(10);
  }
  TEST_ASSERT_INT_WITHIN(1, 30, on);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_duty_030_is_about_30_percent);
  RUN_TEST(test_duty_zero_is_always_off);
  RUN_TEST(test_duty_one_is_always_on);
  RUN_TEST(test_tiny_duty_snaps_off);
  RUN_TEST(test_near_full_duty_snaps_on);
  RUN_TEST(test_duty_latched_per_window);
  RUN_TEST(test_force_off_is_immediate_and_sticky);
  RUN_TEST(test_duty_clamped);
  RUN_TEST(test_wraparound_window);
  return UNITY_END();
}
