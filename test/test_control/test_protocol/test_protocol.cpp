// native_control suite — nanopb round-trip through lib/protocol's codec
// wrappers (proving the .proto -> .pb.c/.pb.h codegen step feeds this env) and
// a TinyFrame link smoke: frame bytes reach a bound ISerialTransport.
#include <cstring>

#include <unity.h>

#include "codec.h"
#include "messages.h"
#include "oven.pb.h"
#include "tf_glue.h"

// Capturing transport: records everything TinyFrame emits.
struct CaptureTransport : ISerialTransport {
  uint8_t bytes[TF_SENDBUF_LEN] = {0};
  size_t len = 0;
  size_t write(const uint8_t *data, size_t n) override {
    size_t room = sizeof(bytes) - len;
    if (n > room) {
      n = room;
    }
    memcpy(bytes + len, data, n);
    len += n;
    return n;
  }
  size_t read(uint8_t *, size_t) override { return 0; }
};

void setUp(void) {}
void tearDown(void) {}

void test_ack_round_trip(void) {
  oven_Ack out = oven_Ack_init_default;
  out.seq = 42;
  uint8_t buf[oven_Ack_size];
  size_t len = 0;
  TEST_ASSERT_TRUE(protocol::encode(oven_Ack_fields, &out, buf, sizeof(buf), len));
  TEST_ASSERT_GREATER_THAN(0, (int)len);

  oven_Ack in = oven_Ack_init_default;
  TEST_ASSERT_TRUE(protocol::decode(oven_Ack_fields, &in, buf, len));
  TEST_ASSERT_EQUAL_UINT32(42, in.seq);
}

void test_decode_rejects_garbage(void) {
  // A truncated varint field is malformed input, not a crash.
  const uint8_t garbage[] = {0x08}; // field 1 varint header with no payload
  oven_Ack in = oven_Ack_init_default;
  TEST_ASSERT_FALSE(protocol::decode(oven_Ack_fields, &in, garbage, sizeof(garbage)));
}

void test_tinyframe_emits_framed_payload(void) {
  static TinyFrame tf;
  TF_InitStatic(&tf, TF_MASTER);
  CaptureTransport transport;
  protocol::bindTransport(tf, transport);

  // Frame an encoded Heartbeat like the reliability layer (A1/A2) will.
  oven_Heartbeat hb = oven_Heartbeat_init_default;
  hb.session = 1;
  hb.seq = 7;
  hb.enable = true;
  uint8_t payload[oven_Heartbeat_size];
  size_t payload_len = 0;
  TEST_ASSERT_TRUE(
      protocol::encode(oven_Heartbeat_fields, &hb, payload, sizeof(payload), payload_len));

  TF_Msg msg;
  TF_ClearMsg(&msg);
  msg.type = protocol::kTfTypeHeartbeat;
  msg.data = payload;
  msg.len = (TF_LEN)payload_len;
  TEST_ASSERT_TRUE(TF_Send(&tf, &msg));

  TEST_ASSERT_GREATER_THAN((int)payload_len, (int)transport.len); // payload + frame overhead
  TEST_ASSERT_EQUAL_HEX8(TF_SOF_BYTE, transport.bytes[0]);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_ack_round_trip);
  RUN_TEST(test_decode_rejects_garbage);
  RUN_TEST(test_tinyframe_emits_framed_payload);
  return UNITY_END();
}
