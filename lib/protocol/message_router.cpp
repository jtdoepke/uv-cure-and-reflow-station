#include "message_router.h"

#include "codec.h"

namespace protocol {

namespace {

// Decode payload into a zero-initialized msg (nanopb's required starting state,
// == *_init_zero; proto3 has no non-zero defaults here) and, on success,
// dispatch via fn. `seed` only carries the message type for deduction. Returns
// false (letting the caller signal onDecodeError) if the payload is malformed.
template <typename Msg, typename Fn>
bool route(const pb_msgdesc_t *fields, Msg seed, const uint8_t *payload, size_t len, Fn fn) {
  Msg msg = seed;
  if (!decode(fields, &msg, payload, len)) {
    return false;
  }
  fn(msg);
  return true;
}

} // namespace

void MessageRouter::onFrame(uint8_t type, const uint8_t *payload, size_t len) {
  if (observer_ == nullptr) {
    return; // not yet wired; drop
  }
  IMessageObserver &obs = *observer_;

  bool ok = true;
  switch (type) {
  case kTfTypeHello:
    ok = route(oven_Hello_fields, oven_Hello{}, payload, len,
               [&](const oven_Hello &m) { obs.onHello(m); });
    break;
  case kTfTypeRecipe:
    ok = route(oven_Recipe_fields, oven_Recipe{}, payload, len,
               [&](const oven_Recipe &m) { obs.onRecipe(m); });
    break;
  case kTfTypeStart:
    ok = route(oven_Start_fields, oven_Start{}, payload, len,
               [&](const oven_Start &m) { obs.onStart(m); });
    break;
  case kTfTypeHeartbeat:
    ok = route(oven_Heartbeat_fields, oven_Heartbeat{}, payload, len,
               [&](const oven_Heartbeat &m) { obs.onHeartbeat(m); });
    break;
  case kTfTypeAbort:
    // Abort carries no fields; a bare frame of this type is the whole message.
    obs.onAbort();
    break;
  case kTfTypeAck:
    ok = route(oven_Ack_fields, oven_Ack{}, payload, len, [&](const oven_Ack &m) { obs.onAck(m); });
    break;
  case kTfTypeNak:
    ok = route(oven_Nak_fields, oven_Nak{}, payload, len, [&](const oven_Nak &m) { obs.onNak(m); });
    break;
  case kTfTypeTelemetry:
    ok = route(oven_Telemetry_fields, oven_Telemetry{}, payload, len,
               [&](const oven_Telemetry &m) { obs.onTelemetry(m); });
    break;
  case kTfTypeDone:
    ok = route(oven_Done_fields, oven_Done{}, payload, len,
               [&](const oven_Done &m) { obs.onDone(m); });
    break;
  case kTfTypeFault:
    ok = route(oven_Fault_fields, oven_Fault{}, payload, len,
               [&](const oven_Fault &m) { obs.onFault(m); });
    break;
  default:
    obs.onUnknownType(type, payload, len);
    return;
  }

  if (!ok) {
    obs.onDecodeError(type);
  }
}

} // namespace protocol
