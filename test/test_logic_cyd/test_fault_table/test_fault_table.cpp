// native_logic_cyd suite — pure host tests of the §22 faultCode → operator-wording table.
// No LVGL/Arduino. The point of putting the copy in app_logic is that the exact §22 wording is
// asserted here rather than reviewed by eye in the view.
#include <cstring>

#include <unity.h>

#include "fault_table.h"

using fault_table::faultInfo;
using fault_table::formatTitle;
using fault_table::outranks;
using fault_table::Severity;

void setUp(void) {}
void tearDown(void) {}

namespace {
// Every code §22's taxonomy defines. FAULT_NONE is excluded deliberately — it is the enum's
// zero value, not a fault (see faultInfo()).
constexpr oven_FaultCode kAllCodes[] = {
    oven_FaultCode_FAULT_OVERTEMP_CHAMBER,   oven_FaultCode_FAULT_OVERTEMP_CASE,
    oven_FaultCode_FAULT_SENSOR_FAULT,       oven_FaultCode_FAULT_TC_IMPLAUSIBLE,
    oven_FaultCode_FAULT_TARGET_UNREACHABLE, oven_FaultCode_FAULT_HEATER_STUCK,
    oven_FaultCode_FAULT_RUNTIME_EXCEEDED,   oven_FaultCode_FAULT_LINK_LOST,
    oven_FaultCode_FAULT_WATCHDOG,           oven_FaultCode_FAULT_INTERNAL,
};
constexpr size_t kAllCodesCount = sizeof(kAllCodes) / sizeof(kAllCodes[0]);
} // namespace

// Catches a missing table row the next time a code is appended to the .proto (§9).
void test_every_known_code_has_nonempty_title_guidance_and_codename(void) {
  for (size_t i = 0; i < kAllCodesCount; ++i) {
    const fault_table::FaultInfo info = faultInfo(kAllCodes[i]);
    TEST_ASSERT_TRUE(info.known);
    TEST_ASSERT_NOT_NULL(info.title);
    TEST_ASSERT_NOT_NULL(info.guidance);
    TEST_ASSERT_NOT_NULL(info.codeName);
    TEST_ASSERT_TRUE(strlen(info.title) > 0);
    TEST_ASSERT_TRUE(strlen(info.guidance) > 0);
    TEST_ASSERT_TRUE(strlen(info.codeName) > 0);
  }
}

// §22: an unrecognised code "shows `Fault <code> — oven safed to a safe state` rather than a
// blank — defensive even though the matched-pair invariant (§9) should keep the tables in sync."
void test_unknown_code_formats_the_generic_title(void) {
  const oven_FaultCode unknown = static_cast<oven_FaultCode>(99);
  TEST_ASSERT_FALSE(faultInfo(unknown).known);
  char buf[64];
  formatTitle(unknown, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("Fault 99 \xE2\x80\x94 oven safed to a safe state", buf);
}

// FAULT_NONE is the zero value, not a fault — it must not masquerade as a known one.
void test_fault_none_is_not_a_known_fault(void) {
  TEST_ASSERT_FALSE(faultInfo(oven_FaultCode_FAULT_NONE).known);
}

void test_known_code_formats_its_table_title(void) {
  char buf[64];
  formatTitle(oven_FaultCode_FAULT_OVERTEMP_CHAMBER, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("Chamber over-temperature", buf);
}

// §22 quotes this string verbatim. Both clauses are load-bearing: the timeout clause covers a
// hung link with a live controller; the reset-default clause covers a controller that isn't
// executing at all. Trimming either turns reassurance-via-invariant into an unbacked claim.
void test_link_lost_guidance_is_the_verbatim_section_22_text(void) {
  const fault_table::FaultInfo info = faultInfo(oven_FaultCode_FAULT_LINK_LOST);
  TEST_ASSERT_EQUAL_STRING(
      "Lost communication with the controller. If a run was active, the heater is off \xE2\x80\x94 "
      "the controller safes on heartbeat timeout, and its outputs default OFF on any reset.",
      info.guidance);
}

void test_format_title_truncates_safely(void) {
  char buf[8];
  memset(buf, 'x', sizeof(buf));
  formatTitle(oven_FaultCode_FAULT_OVERTEMP_CHAMBER, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_CHAR('\0', buf[sizeof(buf) - 1]); // always NUL-terminates
  TEST_ASSERT_EQUAL_STRING("Chamber", buf);
}

void test_format_title_tolerates_a_zero_length_buffer(void) {
  char buf[1] = {'x'};
  formatTitle(oven_FaultCode_FAULT_INTERNAL, buf, 0); // must not write
  TEST_ASSERT_EQUAL_CHAR('x', buf[0]);
  formatTitle(oven_FaultCode_FAULT_INTERNAL, nullptr, 8); // must not crash
}

// The order §22's "higher-priority fault while shown" rule depends on. design.md never defines
// it (`severity` appears once, in the architecture note), so this test pins the B7 decision.
void test_severity_ordering(void) {
  TEST_ASSERT_TRUE(
      outranks(oven_FaultCode_FAULT_HEATER_STUCK, oven_FaultCode_FAULT_OVERTEMP_CHAMBER));
  TEST_ASSERT_TRUE(
      outranks(oven_FaultCode_FAULT_OVERTEMP_CHAMBER, oven_FaultCode_FAULT_SENSOR_FAULT));
  TEST_ASSERT_TRUE(outranks(oven_FaultCode_FAULT_SENSOR_FAULT, oven_FaultCode_FAULT_WATCHDOG));
  TEST_ASSERT_TRUE(outranks(oven_FaultCode_FAULT_WATCHDOG, oven_FaultCode_FAULT_LINK_LOST));
  TEST_ASSERT_TRUE(
      outranks(oven_FaultCode_FAULT_LINK_LOST, oven_FaultCode_FAULT_TARGET_UNREACHABLE));
}

// Strict: §22 shows one cause + a count, so ties need a deterministic winner (first wins).
void test_equal_severity_does_not_outrank(void) {
  TEST_ASSERT_FALSE(
      outranks(oven_FaultCode_FAULT_OVERTEMP_CASE, oven_FaultCode_FAULT_OVERTEMP_CHAMBER));
  TEST_ASSERT_FALSE(
      outranks(oven_FaultCode_FAULT_OVERTEMP_CHAMBER, oven_FaultCode_FAULT_OVERTEMP_CASE));
  TEST_ASSERT_FALSE(
      outranks(oven_FaultCode_FAULT_RUNTIME_EXCEEDED, oven_FaultCode_FAULT_TARGET_UNREACHABLE));
}

// An unknown code ranks with INTERNAL, the existing catch-all: high enough that a genuinely new
// hazard code isn't buried, low enough that a corrupt frame can't preempt a real over-temp.
void test_unknown_code_ranks_with_internal(void) {
  const oven_FaultCode unknown = static_cast<oven_FaultCode>(99);
  TEST_ASSERT_FALSE(outranks(unknown, oven_FaultCode_FAULT_INTERNAL));
  TEST_ASSERT_FALSE(outranks(oven_FaultCode_FAULT_INTERNAL, unknown));
  TEST_ASSERT_TRUE(outranks(oven_FaultCode_FAULT_OVERTEMP_CHAMBER, unknown));
  TEST_ASSERT_TRUE(outranks(unknown, oven_FaultCode_FAULT_LINK_LOST));
}

// §14's HOT band / §17's sleep suppression are chamber-specific. An electronics-case over-temp
// (§6) says nothing about a touchable chamber, so flagging it would be a false HOT.
void test_hot_flag_is_chamber_specific(void) {
  TEST_ASSERT_TRUE(faultInfo(oven_FaultCode_FAULT_OVERTEMP_CHAMBER).overTemp);
  TEST_ASSERT_TRUE(faultInfo(oven_FaultCode_FAULT_HEATER_STUCK).overTemp); // uncommanded heat
  TEST_ASSERT_FALSE(faultInfo(oven_FaultCode_FAULT_OVERTEMP_CASE).overTemp);
  TEST_ASSERT_FALSE(faultInfo(oven_FaultCode_FAULT_LINK_LOST).overTemp);
  TEST_ASSERT_FALSE(faultInfo(oven_FaultCode_FAULT_TARGET_UNREACHABLE).overTemp);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_every_known_code_has_nonempty_title_guidance_and_codename);
  RUN_TEST(test_unknown_code_formats_the_generic_title);
  RUN_TEST(test_fault_none_is_not_a_known_fault);
  RUN_TEST(test_known_code_formats_its_table_title);
  RUN_TEST(test_link_lost_guidance_is_the_verbatim_section_22_text);
  RUN_TEST(test_format_title_truncates_safely);
  RUN_TEST(test_format_title_tolerates_a_zero_length_buffer);
  RUN_TEST(test_severity_ordering);
  RUN_TEST(test_equal_severity_does_not_outrank);
  RUN_TEST(test_unknown_code_ranks_with_internal);
  RUN_TEST(test_hot_flag_is_chamber_specific);
  return UNITY_END();
}
