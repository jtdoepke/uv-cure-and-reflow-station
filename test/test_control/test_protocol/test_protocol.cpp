// native_control suite — nanopb round-trip through lib/protocol's codec
// wrappers, proving the .proto -> .pb.c/.pb.h codegen step feeds this env.
#include <unity.h>

#include "codec.h"
#include "oven.pb.h"

void setUp(void) {}
void tearDown(void) {}

void test_dummy_round_trip(void) {
  oven_Dummy out = oven_Dummy_init_default;
  out.value = 42;
  uint8_t buf[oven_Dummy_size];
  size_t len = 0;
  TEST_ASSERT_TRUE(protocol::encode(oven_Dummy_fields, &out, buf, sizeof(buf), len));
  TEST_ASSERT_GREATER_THAN(0, (int)len);

  oven_Dummy in = oven_Dummy_init_default;
  TEST_ASSERT_TRUE(protocol::decode(oven_Dummy_fields, &in, buf, len));
  TEST_ASSERT_EQUAL_UINT32(42, in.value);
}

void test_decode_rejects_garbage(void) {
  // A truncated varint field is malformed input, not a crash.
  const uint8_t garbage[] = {0x08}; // field 1 varint header with no payload
  oven_Dummy in = oven_Dummy_init_default;
  TEST_ASSERT_FALSE(protocol::decode(oven_Dummy_fields, &in, garbage, sizeof(garbage)));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_dummy_round_trip);
  RUN_TEST(test_decode_rejects_garbage);
  return UNITY_END();
}
