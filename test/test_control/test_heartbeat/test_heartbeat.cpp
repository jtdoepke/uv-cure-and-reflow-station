// native_control suite — pure host tests of HeartbeatMonitor behind the IClock
// port. No Arduino: a FakeClock is injected and advanced by hand.
#include <unity.h>

#include "heartbeat_monitor.h"
#include "helpers/fake_clock.h"

static constexpr uint32_t kWindowMs = 750; // command-timeout from design.md §9

static FakeClock clk;

void setUp(void) {
  clk = FakeClock();
} // reset the double before each test
void tearDown(void) {}

void test_never_fed_is_expired(void) {
  protocol::HeartbeatMonitor hb(clk);
  TEST_ASSERT_TRUE(hb.expired(kWindowMs));
}

void test_fresh_feed_is_not_expired(void) {
  protocol::HeartbeatMonitor hb(clk);
  hb.feed();
  TEST_ASSERT_FALSE(hb.expired(kWindowMs));
  clk.advance(kWindowMs - 1);
  TEST_ASSERT_FALSE(hb.expired(kWindowMs));
}

void test_stale_feed_is_expired(void) {
  protocol::HeartbeatMonitor hb(clk);
  hb.feed();
  clk.advance(kWindowMs);
  TEST_ASSERT_TRUE(hb.expired(kWindowMs));
}

void test_refeed_renews_the_window(void) {
  protocol::HeartbeatMonitor hb(clk);
  hb.feed();
  clk.advance(kWindowMs - 1);
  hb.feed();
  clk.advance(kWindowMs - 1);
  TEST_ASSERT_FALSE(hb.expired(kWindowMs));
}

void test_survives_millis_wraparound(void) {
  protocol::HeartbeatMonitor hb(clk);
  clk.now = 0xFFFFFF00u; // 256 ms before wrap
  hb.feed();
  clk.now = 0x000000FFu; // 511 ms later, past the wrap
  TEST_ASSERT_FALSE(hb.expired(kWindowMs));
  clk.advance(kWindowMs);
  TEST_ASSERT_TRUE(hb.expired(kWindowMs));
}

void test_reset_returns_to_expired(void) {
  protocol::HeartbeatMonitor hb(clk);
  hb.feed();
  hb.reset();
  TEST_ASSERT_TRUE(hb.expired(kWindowMs));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_never_fed_is_expired);
  RUN_TEST(test_fresh_feed_is_not_expired);
  RUN_TEST(test_stale_feed_is_expired);
  RUN_TEST(test_refeed_renews_the_window);
  RUN_TEST(test_survives_millis_wraparound);
  RUN_TEST(test_reset_returns_to_expired);
  return UNITY_END();
}
