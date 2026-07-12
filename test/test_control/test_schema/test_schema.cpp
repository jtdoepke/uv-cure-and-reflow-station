// native_control suite — v1 schema round-trips + the generated schema-hash
// constant (design.md §9). Exercises the oven.options static bounds: a Recipe
// with segments and a Telemetry with a full wall_temp array must round-trip.
#include <unity.h>

#include "codec.h"
#include "messages.h"
#include "schema.h"

void setUp(void) {}
void tearDown(void) {}

void test_schema_hash_is_nonzero(void) {
  TEST_ASSERT_NOT_EQUAL(0, (int)(protocol::kSchemaHash != 0));
  TEST_ASSERT_EQUAL_UINT32(1, protocol::kProtoVer);
}

void test_hello_round_trip(void) {
  oven_Hello out = oven_Hello_init_default;
  out.proto_ver = protocol::kProtoVer;
  out.schema_hash = protocol::kSchemaHash;
  uint8_t buf[oven_Hello_size];
  size_t len = 0;
  TEST_ASSERT_TRUE(protocol::encode(oven_Hello_fields, &out, buf, sizeof(buf), len));

  oven_Hello in = oven_Hello_init_default;
  TEST_ASSERT_TRUE(protocol::decode(oven_Hello_fields, &in, buf, len));
  TEST_ASSERT_EQUAL_UINT32(protocol::kProtoVer, in.proto_ver);
  TEST_ASSERT_TRUE(protocol::kSchemaHash == in.schema_hash);
}

void test_recipe_round_trip_with_segments(void) {
  oven_Recipe out = oven_Recipe_init_default;
  out.id = 7;
  out.mode = oven_Mode_MODE_CURE;
  out.segments_count = 2;
  out.segments[0].dur_ms = 60000;
  out.segments[0].heat_c = 60.0f;
  out.segments[0].interp = oven_Interp_INTERP_RAMP_ASAP;
  out.segments[0].uv = false;
  out.segments[1].dur_ms = 300000;
  out.segments[1].heat_c = 60.0f;
  out.segments[1].interp = oven_Interp_INTERP_HOLD;
  out.segments[1].uv = true;

  uint8_t buf[oven_Recipe_size];
  size_t len = 0;
  TEST_ASSERT_TRUE(protocol::encode(oven_Recipe_fields, &out, buf, sizeof(buf), len));

  oven_Recipe in = oven_Recipe_init_default;
  TEST_ASSERT_TRUE(protocol::decode(oven_Recipe_fields, &in, buf, len));
  TEST_ASSERT_EQUAL_UINT32(7, in.id);
  TEST_ASSERT_EQUAL(oven_Mode_MODE_CURE, in.mode);
  TEST_ASSERT_EQUAL(2, in.segments_count);
  TEST_ASSERT_EQUAL_UINT32(300000, in.segments[1].dur_ms);
  TEST_ASSERT_TRUE(in.segments[1].uv);
  TEST_ASSERT_EQUAL(oven_Interp_INTERP_HOLD, in.segments[1].interp);
}

void test_telemetry_round_trip_with_full_wall_temp(void) {
  oven_Telemetry out = oven_Telemetry_init_default;
  out.session = 3;
  out.wall_temp_count = 4; // the oven.options max_count
  for (int i = 0; i < 4; i++) {
    out.wall_temp[i] = 100.0f + (float)i;
  }
  out.run_state = oven_RunState_RUN_STATE_RUNNING;
  out.fault_code = oven_FaultCode_FAULT_NONE;
  out.heater_duty = 0.42f;

  uint8_t buf[oven_Telemetry_size];
  size_t len = 0;
  TEST_ASSERT_TRUE(protocol::encode(oven_Telemetry_fields, &out, buf, sizeof(buf), len));

  oven_Telemetry in = oven_Telemetry_init_default;
  TEST_ASSERT_TRUE(protocol::decode(oven_Telemetry_fields, &in, buf, len));
  TEST_ASSERT_EQUAL_UINT32(3, in.session);
  TEST_ASSERT_EQUAL(4, in.wall_temp_count);
  TEST_ASSERT_EQUAL_FLOAT(103.0f, in.wall_temp[3]);
  TEST_ASSERT_EQUAL(oven_RunState_RUN_STATE_RUNNING, in.run_state);
  TEST_ASSERT_EQUAL_FLOAT(0.42f, in.heater_duty);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_schema_hash_is_nonzero);
  RUN_TEST(test_hello_round_trip);
  RUN_TEST(test_recipe_round_trip_with_segments);
  RUN_TEST(test_telemetry_round_trip_with_full_wall_temp);
  return UNITY_END();
}
