// native_control suite — the profile executor (design.md §5, backlog A6).
//
// Drives ProfileExecutor over a FakeClock with a hand-authored temperature trajectory
// and asserts the emitted setpoint / channels / run-state and the per-segment watchdog.
// A final seam test drives an accepted Recipe+Start through a real ControllerLink and
// checks the executor starts (and Abort stops it).
#include <unity.h>

#include "controller_link.h"
#include "frame_link.h"
#include "helpers/fake_clock.h"
#include "helpers/pipe_transport.h"
#include "link_params.h"
#include "message_router.h"
#include "messages.h"
#include "oven.pb.h"
#include "profile_executor.h"
#include "schema.h"

namespace {

oven_Segment makeSeg(oven_Interp interp, float heatC, uint32_t durMs, bool uv = false,
                     bool motor = false, bool convFan = false) {
  oven_Segment s = oven_Segment_init_zero;
  s.interp = interp;
  s.heat_c = heatC;
  s.dur_ms = durMs;
  s.uv = uv;
  s.motor = motor;
  s.conv_fan = convFan;
  return s;
}

oven_Recipe makeRecipe(const oven_Segment *segs, pb_size_t n, uint32_t id = 1,
                       oven_Mode mode = oven_Mode_MODE_REFLOW) {
  oven_Recipe r = oven_Recipe_init_zero;
  r.id = id;
  r.mode = mode;
  r.seq = 1;
  r.segments_count = n;
  for (pb_size_t i = 0; i < n && i < 32; ++i) {
    r.segments[i] = segs[i];
  }
  return r;
}

constexpr oven_RunState kRunning = oven_RunState_RUN_STATE_RUNNING;
constexpr oven_RunState kDone = oven_RunState_RUN_STATE_DONE;
constexpr oven_RunState kFault = oven_RunState_RUN_STATE_FAULT;
constexpr oven_RunState kIdle = oven_RunState_RUN_STATE_IDLE;

} // namespace

void setUp(void) {}
void tearDown(void) {}

// RAMP_OVER_TIME sweeps the setpoint linearly from the start temp to target over dur_ms,
// then advances (and, being the only segment, completes).
void test_ramp_over_time_sweeps_and_advances(void) {
  FakeClock clk;
  ProfileExecutor exec(clk);
  oven_Segment segs[] = {makeSeg(oven_Interp_INTERP_RAMP_OVER_TIME, 100.0f, 10000)};
  exec.load(makeRecipe(segs, 1), /*holdEntryGated=*/true);
  exec.start();

  exec.tick(25.0f, true); // t=0: origin is the measured temp
  TEST_ASSERT_EQUAL(kRunning, exec.state());
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 25.0f, exec.output().setpointC);

  clk.advance(5000);
  exec.tick(40.0f, true); // halfway: 25 -> 100 => 62.5
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 62.5f, exec.output().setpointC);

  clk.advance(5000);
  exec.tick(90.0f, true); // dur elapsed -> advance -> backup cooldown (still hot, heater off, §6)
  TEST_ASSERT_EQUAL(kRunning, exec.state());
  TEST_ASSERT_EQUAL_FLOAT(0.0f, exec.output().setpointC);

  exec.tick(30.0f, true); // measured touch-safe -> DONE
  TEST_ASSERT_EQUAL(kDone, exec.state());
  TEST_ASSERT_TRUE(exec.output().safe);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, exec.output().setpointC);
}

// RAMP_ASAP commands the target and advances only when the measured temp arrives.
void test_ramp_asap_advances_on_reaching_target(void) {
  FakeClock clk;
  ProfileExecutor exec(clk);
  oven_Segment segs[] = {makeSeg(oven_Interp_INTERP_RAMP_ASAP, 100.0f, 60000)};
  exec.load(makeRecipe(segs, 1), true);
  exec.start();

  exec.tick(25.0f, true);
  TEST_ASSERT_EQUAL(kRunning, exec.state());
  TEST_ASSERT_EQUAL_FLOAT(100.0f, exec.output().setpointC); // full-power intent

  clk.advance(5000);
  exec.tick(60.0f, true);
  TEST_ASSERT_EQUAL(kRunning, exec.state()); // not there yet

  clk.advance(5000);
  exec.tick(99.0f, true); // within the 2 C band -> reached -> backup cooldown (still hot, §6)
  TEST_ASSERT_EQUAL(kRunning, exec.state());
  exec.tick(40.0f, true); // touch-safe -> DONE
  TEST_ASSERT_EQUAL(kDone, exec.state());
}

// Reflow HOLD: the hold timer does not start until the measured temp arrives.
void test_hold_gated_waits_for_arrival(void) {
  FakeClock clk;
  ProfileExecutor exec(clk);
  oven_Segment segs[] = {makeSeg(oven_Interp_INTERP_HOLD, 100.0f, 5000)};
  exec.load(makeRecipe(segs, 1), /*holdEntryGated=*/true);
  exec.start();

  exec.tick(25.0f, true);
  clk.advance(5000);
  exec.tick(25.0f, true); // 5 s cold: hold timer must NOT have started
  TEST_ASSERT_EQUAL(kRunning, exec.state());

  exec.tick(100.0f, true); // arrives -> hold begins now
  clk.advance(5000);
  exec.tick(100.0f, true); // full hold elapsed -> backup cooldown (still hot, §6)
  TEST_ASSERT_EQUAL(kRunning, exec.state());
  exec.tick(42.0f, true); // touch-safe -> DONE
  TEST_ASSERT_EQUAL(kDone, exec.state());
}

// Cure HOLD (ungated): the hold is a dose timer — it starts immediately, regardless of
// whether the measured temp reached target.
void test_hold_ungated_starts_immediately(void) {
  FakeClock clk;
  ProfileExecutor exec(clk);
  oven_Segment segs[] = {makeSeg(oven_Interp_INTERP_HOLD, 80.0f, 5000)};
  exec.load(makeRecipe(segs, 1), /*holdEntryGated=*/false);
  exec.start();

  exec.tick(25.0f, true);
  clk.advance(5000);
  exec.tick(25.0f, true); // never reached 80, but time-based -> DONE
  TEST_ASSERT_EQUAL(kDone, exec.state());
}

// Watchdog path (a): a target-gated wait that outruns k x its projected dur_ms faults.
void test_watchdog_k_timeout_faults(void) {
  FakeClock clk;
  ProfileExecutor::Config cfg;
  cfg.watchdogK = 3.0f;
  cfg.rateFloorWindowMs = 100000000u; // disable the rate-floor path for this test
  cfg.maxWaitMs = 100000000u;         // disable the absolute cap for this test
  ProfileExecutor exec(clk, cfg);
  oven_Segment segs[] = {makeSeg(oven_Interp_INTERP_RAMP_ASAP, 200.0f, 10000)};
  exec.load(makeRecipe(segs, 1), true);
  exec.start();

  exec.tick(25.0f, true); // t=0: segment entry / watchdog baseline
  clk.advance(29999);
  exec.tick(25.0f, true); // just under k x dur (30000) -> still trying
  TEST_ASSERT_EQUAL(kRunning, exec.state());

  clk.advance(2);
  exec.tick(25.0f, true); // past 30000 -> TARGET_UNREACHABLE
  TEST_ASSERT_EQUAL(kFault, exec.state());
  TEST_ASSERT_EQUAL(oven_FaultCode_FAULT_TARGET_UNREACHABLE, exec.output().fault);
  TEST_ASSERT_TRUE(exec.output().safe);
}

// Watchdog path (b): a wait where the measured heat rate stays below the floor across a
// full window faults, even before any k x dur bound.
void test_watchdog_rate_floor_faults(void) {
  FakeClock clk;
  ProfileExecutor::Config cfg;
  cfg.rateFloorCPerS = 0.05f;
  cfg.rateFloorWindowMs = 30000;
  cfg.maxWaitMs = 100000000u; // isolate the rate-floor path from the absolute cap
  ProfileExecutor exec(clk, cfg);
  // Huge dur_ms so the k x dur bound can't be what trips it.
  oven_Segment segs[] = {makeSeg(oven_Interp_INTERP_RAMP_ASAP, 200.0f, 100000000u)};
  exec.load(makeRecipe(segs, 1), true);
  exec.start();

  exec.tick(25.0f, true);
  clk.advance(30001);
  exec.tick(25.2f, true); // +0.2 C over 30 s = 0.0067 C/s < 0.05 floor -> fault
  TEST_ASSERT_EQUAL(kFault, exec.state());
  TEST_ASSERT_EQUAL(oven_FaultCode_FAULT_TARGET_UNREACHABLE, exec.output().fault);
}

// A healthy multi-segment run walks to DONE, never faults, and reports the active
// segment's channel states while running.
void test_healthy_multisegment_run(void) {
  FakeClock clk;
  ProfileExecutor exec(clk);
  oven_Segment segs[] = {
      makeSeg(oven_Interp_INTERP_RAMP_ASAP, 80.0f, 20000, /*uv=*/false, /*motor=*/false,
              /*convFan=*/true),
      makeSeg(oven_Interp_INTERP_HOLD, 80.0f, 10000, /*uv=*/true, /*motor=*/true),
  };
  exec.load(makeRecipe(segs, 2, /*id=*/1, oven_Mode_MODE_CURE), /*holdEntryGated=*/false);
  exec.start();

  exec.tick(25.0f, true);
  TEST_ASSERT_EQUAL(0u, exec.output().segIdx);
  TEST_ASSERT_TRUE(exec.output().convFan);
  TEST_ASSERT_FALSE(exec.output().uv);

  clk.advance(2000);
  exec.tick(80.0f, true); // reached -> advance into the hold segment
  TEST_ASSERT_EQUAL(kRunning, exec.state());
  TEST_ASSERT_EQUAL(1u, exec.output().segIdx);
  TEST_ASSERT_TRUE(exec.output().uv);
  TEST_ASSERT_TRUE(exec.output().motor);

  exec.tick(80.0f, true); // enters the hold segment -> its (ungated) timer starts now
  clk.advance(10000);
  exec.tick(80.0f, true); // hold elapsed -> backup cooldown (still hot, §6)
  TEST_ASSERT_EQUAL(kRunning, exec.state());
  TEST_ASSERT_FALSE(exec.output().uv); // channels drop during the cooldown too

  exec.tick(35.0f, true); // touch-safe -> DONE
  TEST_ASSERT_EQUAL(kDone, exec.state());
  TEST_ASSERT_FALSE(exec.output().uv); // channels drop when not running
}

// Backup cooldown (§6): after the last segment, the executor holds the heater off and stays RUNNING
// until the MEASURED temp confirms touch-safe (43 C) — independent of the compiled cool tail.
void test_backup_cooldown_waits_for_touch_safe(void) {
  FakeClock clk;
  ProfileExecutor exec(clk);
  oven_Segment segs[] = {makeSeg(oven_Interp_INTERP_HOLD, 200.0f, 5000)};
  exec.load(makeRecipe(segs, 1), /*holdEntryGated=*/false);
  exec.start();

  exec.tick(200.0f, true);
  clk.advance(5000);
  exec.tick(200.0f, true); // hold elapsed -> enters backup cooldown, still hot
  TEST_ASSERT_EQUAL(kRunning, exec.state());
  TEST_ASSERT_EQUAL_FLOAT(0.0f, exec.output().setpointC); // heater off throughout the cooldown

  clk.advance(60000);
  exec.tick(60.0f, true); // still above 43 -> not done yet
  TEST_ASSERT_EQUAL(kRunning, exec.state());

  clk.advance(60000);
  exec.tick(43.0f, true); // reaches touch-safe -> DONE
  TEST_ASSERT_EQUAL(kDone, exec.state());
  TEST_ASSERT_TRUE(exec.output().safe);
}

// The cooldown wait has a generous backstop: the heater has been off throughout, so on expiry the
// run reports DONE anyway rather than hanging (a genuinely stuck-hot chamber is the L3 trip's job).
void test_backup_cooldown_backstop_completes(void) {
  FakeClock clk;
  ProfileExecutor::Config cfg;
  cfg.cooldownMaxMs = 100000; // short backstop for the test
  ProfileExecutor exec(clk, cfg);
  oven_Segment segs[] = {makeSeg(oven_Interp_INTERP_HOLD, 200.0f, 5000)};
  exec.load(makeRecipe(segs, 1), /*holdEntryGated=*/false);
  exec.start();

  exec.tick(200.0f, true);
  clk.advance(5000);
  exec.tick(200.0f, true); // -> backup cooldown, still hot
  TEST_ASSERT_EQUAL(kRunning, exec.state());

  clk.advance(100001);
  exec.tick(200.0f, true); // never cooled, but the backstop elapsed -> DONE
  TEST_ASSERT_EQUAL(kDone, exec.state());
  TEST_ASSERT_TRUE(exec.output().safe);
}

// A faulted control TC is a stop condition: the executor faults safe (§4).
void test_sensor_fault_is_safe(void) {
  FakeClock clk;
  ProfileExecutor exec(clk);
  oven_Segment segs[] = {makeSeg(oven_Interp_INTERP_HOLD, 80.0f, 60000)};
  exec.load(makeRecipe(segs, 1), false);
  exec.start();
  exec.tick(80.0f, true);
  TEST_ASSERT_EQUAL(kRunning, exec.state());

  exec.tick(0.0f, /*controlValid=*/false);
  TEST_ASSERT_EQUAL(kFault, exec.state());
  TEST_ASSERT_EQUAL(oven_FaultCode_FAULT_SENSOR_FAULT, exec.output().fault);
  TEST_ASSERT_TRUE(exec.output().safe);
}

// abort() drops a run back to a safe IDLE.
void test_abort_returns_to_idle(void) {
  FakeClock clk;
  ProfileExecutor exec(clk);
  oven_Segment segs[] = {makeSeg(oven_Interp_INTERP_HOLD, 80.0f, 60000)};
  exec.load(makeRecipe(segs, 1), false);
  exec.start();
  exec.tick(80.0f, true);
  TEST_ASSERT_EQUAL(kRunning, exec.state());

  exec.abort();
  TEST_ASSERT_EQUAL(kIdle, exec.state());
  TEST_ASSERT_TRUE(exec.output().safe);
}

// A zero-segment recipe completes immediately on start.
void test_zero_segment_recipe_completes(void) {
  FakeClock clk;
  ProfileExecutor exec(clk);
  exec.load(makeRecipe(nullptr, 0), false);
  exec.start();
  TEST_ASSERT_EQUAL(kDone, exec.state());
}

// The setpoint is clamped to the recipe's own maximum target: a segment can never push
// the emitted setpoint above the highest authored heat_c (which A7 bounded to hard-max).
void test_setpoint_clamped_to_recipe_max(void) {
  FakeClock clk;
  ProfileExecutor exec(clk);
  // A descending ramp starting from a measured temp well above the max target.
  oven_Segment segs[] = {makeSeg(oven_Interp_INTERP_RAMP_OVER_TIME, 100.0f, 10000)};
  exec.load(makeRecipe(segs, 1), true);
  exec.start();
  exec.tick(250.0f, true); // origin 250 C, but the recipe max is 100 C
  TEST_ASSERT_TRUE(exec.output().setpointC <= 100.0f + 0.01f);
}

// Seam: an accepted Recipe+Start over a real ControllerLink starts the executor, and a
// mismatched recipe_id does not; Abort stops it.
void test_link_seam_starts_and_aborts(void) {
  LoopbackPipe pipe;
  protocol::IMessageObserver sink;
  protocol::MessageRouter router(sink);
  FakeClock clk;
  protocol::FrameLink link(pipe.a(), TF_MASTER, router);
  ControllerLink ctrl(link, clk);
  ProfileExecutor exec(clk);
  ctrl.setExecutor(exec);

  oven_Recipe r = oven_Recipe_init_zero;
  r.id = 7;
  r.mode = oven_Mode_MODE_REFLOW;
  r.seq = 1;
  r.segments_count = 1;
  r.segments[0] = makeSeg(oven_Interp_INTERP_HOLD, 100.0f, 5000);
  ctrl.onRecipe(r); // validated (accept-all default) -> loaded into the executor
  TEST_ASSERT_EQUAL(kIdle, exec.state());

  oven_Start bad = oven_Start_init_zero;
  bad.session = 1;
  bad.recipe_id = 99; // wrong id
  bad.seq = 2;
  ctrl.onStart(bad);
  TEST_ASSERT_EQUAL(kIdle, exec.state()); // mismatch -> not started

  oven_Start good = oven_Start_init_zero;
  good.session = 1;
  good.recipe_id = 7;
  good.seq = 3;
  ctrl.onStart(good);
  TEST_ASSERT_EQUAL(kRunning, exec.state());

  ctrl.onAbort();
  TEST_ASSERT_EQUAL(kIdle, exec.state());
}

// output().elapsedMs tracks time since start() into the whole run (§15): 0 before start, measured
// from start() (not boot), growing while RUNNING, and back to 0 after abort. This is what the CYD's
// ETA/progress + projection-vs-actual alignment read — it was previously never populated, so the
// Run screen's ETA froze and the projection was read at t=0 (ambient), tripping a false deviation.
void test_elapsed_ms_tracks_run(void) {
  FakeClock clk;
  ProfileExecutor exec(clk);
  oven_Segment segs[] = {makeSeg(oven_Interp_INTERP_RAMP_OVER_TIME, 100.0f, 60000)};
  exec.load(makeRecipe(segs, 1), /*holdEntryGated=*/true);
  TEST_ASSERT_EQUAL_UINT32(0, exec.output().elapsedMs); // idle before start

  clk.advance(5000); // time passes before the run begins
  exec.start();
  TEST_ASSERT_EQUAL_UINT32(0, exec.output().elapsedMs); // measured from start(), not boot

  clk.advance(3000);
  exec.tick(40.0f, true);
  TEST_ASSERT_EQUAL_UINT32(3000, exec.output().elapsedMs);

  clk.advance(4000);
  exec.tick(70.0f, true);
  TEST_ASSERT_EQUAL(kRunning, exec.state());
  TEST_ASSERT_EQUAL_UINT32(7000, exec.output().elapsedMs);

  exec.abort();
  TEST_ASSERT_EQUAL_UINT32(0, exec.output().elapsedMs); // back to idle
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_ramp_over_time_sweeps_and_advances);
  RUN_TEST(test_elapsed_ms_tracks_run);
  RUN_TEST(test_ramp_asap_advances_on_reaching_target);
  RUN_TEST(test_hold_gated_waits_for_arrival);
  RUN_TEST(test_hold_ungated_starts_immediately);
  RUN_TEST(test_watchdog_k_timeout_faults);
  RUN_TEST(test_watchdog_rate_floor_faults);
  RUN_TEST(test_healthy_multisegment_run);
  RUN_TEST(test_backup_cooldown_waits_for_touch_safe);
  RUN_TEST(test_backup_cooldown_backstop_completes);
  RUN_TEST(test_sensor_fault_is_safe);
  RUN_TEST(test_abort_returns_to_idle);
  RUN_TEST(test_zero_segment_recipe_completes);
  RUN_TEST(test_setpoint_clamped_to_recipe_max);
  RUN_TEST(test_link_seam_starts_and_aborts);
  return UNITY_END();
}
