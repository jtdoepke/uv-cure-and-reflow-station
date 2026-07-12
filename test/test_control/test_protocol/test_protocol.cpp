// native_control suite — lib/protocol framing end to end. Two FrameLinks wired
// together through an in-memory LoopbackPipe exercise the full path a real link
// takes: encode (nanopb) -> frame (TinyFrame) -> transport -> parse -> dispatch
// -> decode. Plus the pure-codec round-trip that proves the .proto -> .pb.c/.pb.h
// codegen feeds this env.
#include <cstring>

#include <unity.h>

#include "codec.h"
#include "frame_link.h"
#include "helpers/pipe_transport.h"
#include "messages.h"
#include "oven.pb.h"

// Records the last frame dispatched to it, plus a call count.
struct RecordingHandler : protocol::IFrameHandler {
  int calls = 0;
  uint8_t last_type = 0;
  uint8_t last_payload[TF_MAX_PAYLOAD_RX] = {0};
  size_t last_len = 0;
  void onFrame(uint8_t type, const uint8_t *payload, size_t len) override {
    ++calls;
    last_type = type;
    last_len = len;
    if (len <= sizeof(last_payload)) {
      memcpy(last_payload, payload, len);
    }
  }
};

void setUp(void) {}
void tearDown(void) {}

// --- pure codec (no framing) ---

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

// --- framing through the pipe ---

// Frame a Heartbeat on the CYD side, pump the controller side, and assert the
// dispatched payload decodes back to the same message.
void test_frame_round_trip_through_pipe(void) {
  LoopbackPipe pipe;
  RecordingHandler ctrl_rx;
  RecordingHandler cyd_rx;
  protocol::FrameLink cyd(pipe.a(), TF_MASTER, cyd_rx);
  protocol::FrameLink ctrl(pipe.b(), TF_SLAVE, ctrl_rx);

  oven_Heartbeat hb = oven_Heartbeat_init_default;
  hb.session = 1;
  hb.seq = 7;
  hb.enable = true;
  uint8_t payload[oven_Heartbeat_size];
  size_t payload_len = 0;
  TEST_ASSERT_TRUE(
      protocol::encode(oven_Heartbeat_fields, &hb, payload, sizeof(payload), payload_len));

  TEST_ASSERT_TRUE(cyd.send(protocol::kTfTypeHeartbeat, payload, payload_len));
  ctrl.poll();

  TEST_ASSERT_EQUAL_INT(1, ctrl_rx.calls);
  TEST_ASSERT_EQUAL_HEX8(protocol::kTfTypeHeartbeat, ctrl_rx.last_type);

  oven_Heartbeat got = oven_Heartbeat_init_default;
  TEST_ASSERT_TRUE(
      protocol::decode(oven_Heartbeat_fields, &got, ctrl_rx.last_payload, ctrl_rx.last_len));
  TEST_ASSERT_EQUAL_UINT32(1, got.session);
  TEST_ASSERT_EQUAL_UINT32(7, got.seq);
  TEST_ASSERT_TRUE(got.enable);
}

// The controller can frame a reply back to the CYD over the same pipe.
void test_frame_round_trip_reverse(void) {
  LoopbackPipe pipe;
  RecordingHandler ctrl_rx;
  RecordingHandler cyd_rx;
  protocol::FrameLink cyd(pipe.a(), TF_MASTER, cyd_rx);
  protocol::FrameLink ctrl(pipe.b(), TF_SLAVE, ctrl_rx);

  oven_Ack ack = oven_Ack_init_default;
  ack.seq = 99;
  uint8_t payload[oven_Ack_size];
  size_t payload_len = 0;
  TEST_ASSERT_TRUE(protocol::encode(oven_Ack_fields, &ack, payload, sizeof(payload), payload_len));

  TEST_ASSERT_TRUE(ctrl.send(protocol::kTfTypeAck, payload, payload_len));
  cyd.poll();

  TEST_ASSERT_EQUAL_INT(1, cyd_rx.calls);
  TEST_ASSERT_EQUAL_HEX8(protocol::kTfTypeAck, cyd_rx.last_type);
  TEST_ASSERT_EQUAL_INT(0, ctrl_rx.calls); // reply did not echo back to the sender

  oven_Ack got = oven_Ack_init_default;
  TEST_ASSERT_TRUE(protocol::decode(oven_Ack_fields, &got, cyd_rx.last_payload, cyd_rx.last_len));
  TEST_ASSERT_EQUAL_UINT32(99, got.seq);
}

// The byte pump reassembles a frame split across many poll() calls (a UART
// delivers bytes in arbitrary chunks).
void test_frame_reassembles_across_chunked_delivery(void) {
  LoopbackPipe pipe;
  RecordingHandler ctrl_rx;
  RecordingHandler cyd_rx;
  protocol::FrameLink cyd(pipe.a(), TF_MASTER, cyd_rx);
  protocol::FrameLink ctrl(pipe.b(), TF_SLAVE, ctrl_rx);

  oven_Heartbeat hb = oven_Heartbeat_init_default;
  hb.session = 3;
  hb.seq = 4;
  uint8_t payload[oven_Heartbeat_size];
  size_t payload_len = 0;
  TEST_ASSERT_TRUE(
      protocol::encode(oven_Heartbeat_fields, &hb, payload, sizeof(payload), payload_len));
  TEST_ASSERT_TRUE(cyd.send(protocol::kTfTypeHeartbeat, payload, payload_len));

  // Pull the full frame off the wire, then feed it back one byte at a time.
  uint8_t frame[TF_SENDBUF_LEN];
  size_t total = pipe.b().read(frame, sizeof(frame));
  TEST_ASSERT_GREATER_THAN(0, (int)total);

  for (size_t i = 0; i < total; ++i) {
    pipe.injectAToB(&frame[i], 1);
    ctrl.poll();
    if (i + 1 < total) {
      TEST_ASSERT_EQUAL_INT(0, ctrl_rx.calls); // no dispatch until the last byte
    }
  }
  TEST_ASSERT_EQUAL_INT(1, ctrl_rx.calls);
}

// A single corrupted byte fails the CRC and the frame is dropped, never
// dispatched (design.md §9: bad CRC -> treated as a missed tick).
void test_corrupted_frame_is_dropped(void) {
  LoopbackPipe pipe;
  RecordingHandler ctrl_rx;
  RecordingHandler cyd_rx;
  protocol::FrameLink cyd(pipe.a(), TF_MASTER, cyd_rx);
  protocol::FrameLink ctrl(pipe.b(), TF_SLAVE, ctrl_rx);

  oven_Heartbeat hb = oven_Heartbeat_init_default;
  hb.session = 5;
  uint8_t payload[oven_Heartbeat_size];
  size_t payload_len = 0;
  TEST_ASSERT_TRUE(
      protocol::encode(oven_Heartbeat_fields, &hb, payload, sizeof(payload), payload_len));
  TEST_ASSERT_TRUE(cyd.send(protocol::kTfTypeHeartbeat, payload, payload_len));

  uint8_t frame[TF_SENDBUF_LEN];
  size_t total = pipe.b().read(frame, sizeof(frame));
  TEST_ASSERT_GREATER_THAN(2, (int)total);

  frame[total / 2] ^= 0xFF; // flip a mid-frame byte -> checksum mismatch
  pipe.injectAToB(frame, total);
  ctrl.poll();

  TEST_ASSERT_EQUAL_INT(0, ctrl_rx.calls);
}

// The generic dispatch is type-agnostic: an unknown frame type still reaches the
// handler with its id intact — this is the A1/A2 seam (A2 decides what types mean).
void test_unknown_type_still_dispatches(void) {
  LoopbackPipe pipe;
  RecordingHandler ctrl_rx;
  RecordingHandler cyd_rx;
  protocol::FrameLink cyd(pipe.a(), TF_MASTER, cyd_rx);
  protocol::FrameLink ctrl(pipe.b(), TF_SLAVE, ctrl_rx);

  const uint8_t body[] = {0xDE, 0xAD};
  const uint8_t kUnknownType = 0x99;
  TEST_ASSERT_TRUE(cyd.send(kUnknownType, body, sizeof(body)));
  ctrl.poll();

  TEST_ASSERT_EQUAL_INT(1, ctrl_rx.calls);
  TEST_ASSERT_EQUAL_HEX8(kUnknownType, ctrl_rx.last_type);
  TEST_ASSERT_EQUAL_size_t(sizeof(body), ctrl_rx.last_len);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(body, ctrl_rx.last_payload, sizeof(body));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_ack_round_trip);
  RUN_TEST(test_decode_rejects_garbage);
  RUN_TEST(test_frame_round_trip_through_pipe);
  RUN_TEST(test_frame_round_trip_reverse);
  RUN_TEST(test_frame_reassembles_across_chunked_delivery);
  RUN_TEST(test_corrupted_frame_is_dropped);
  RUN_TEST(test_unknown_type_still_dispatches);
  return UNITY_END();
}
