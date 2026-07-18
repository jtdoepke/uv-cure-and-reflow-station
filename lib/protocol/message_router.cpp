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
  // Profile & settings management (design.md §9; added 2026-07-17).
  case kTfTypeProfileListReq:
    ok = route(oven_ProfileListReq_fields, oven_ProfileListReq{}, payload, len,
               [&](const oven_ProfileListReq &m) { obs.onProfileListReq(m); });
    break;
  case kTfTypeProfileGetReq:
    ok = route(oven_ProfileGetReq_fields, oven_ProfileGetReq{}, payload, len,
               [&](const oven_ProfileGetReq &m) { obs.onProfileGetReq(m); });
    break;
  case kTfTypeProfilePut:
    ok = route(oven_ProfilePut_fields, oven_ProfilePut{}, payload, len,
               [&](const oven_ProfilePut &m) { obs.onProfilePut(m); });
    break;
  case kTfTypeProfileDelete:
    ok = route(oven_ProfileDelete_fields, oven_ProfileDelete{}, payload, len,
               [&](const oven_ProfileDelete &m) { obs.onProfileDelete(m); });
    break;
  case kTfTypeProfileDup:
    ok = route(oven_ProfileDup_fields, oven_ProfileDup{}, payload, len,
               [&](const oven_ProfileDup &m) { obs.onProfileDup(m); });
    break;
  case kTfTypeProfileRename:
    ok = route(oven_ProfileRename_fields, oven_ProfileRename{}, payload, len,
               [&](const oven_ProfileRename &m) { obs.onProfileRename(m); });
    break;
  case kTfTypeSettingsGetReq:
    ok = route(oven_SettingsGetReq_fields, oven_SettingsGetReq{}, payload, len,
               [&](const oven_SettingsGetReq &m) { obs.onSettingsGetReq(m); });
    break;
  case kTfTypeSettingsPut:
    ok = route(oven_SettingsPut_fields, oven_SettingsPut{}, payload, len,
               [&](const oven_SettingsPut &m) { obs.onSettingsPut(m); });
    break;
  case kTfTypeProfileList:
    ok = route(oven_ProfileList_fields, oven_ProfileList{}, payload, len,
               [&](const oven_ProfileList &m) { obs.onProfileList(m); });
    break;
  case kTfTypeProfileData:
    ok = route(oven_ProfileData_fields, oven_ProfileData{}, payload, len,
               [&](const oven_ProfileData &m) { obs.onProfileData(m); });
    break;
  case kTfTypeSettingsData:
    ok = route(oven_SettingsData_fields, oven_SettingsData{}, payload, len,
               [&](const oven_SettingsData &m) { obs.onSettingsData(m); });
    break;
  case kTfTypeMgmtResult:
    ok = route(oven_MgmtResult_fields, oven_MgmtResult{}, payload, len,
               [&](const oven_MgmtResult &m) { obs.onMgmtResult(m); });
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
