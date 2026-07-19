// MessageRouter — the typed seam over A1's raw framing (design.md §9).
//
// FrameLink hands up raw (type, payload) pairs and knows nothing about their
// meaning. MessageRouter is an IFrameHandler that switches on the kTfType* id,
// decodes the payload into the matching nanopb struct (protocol::decode), and
// dispatches a typed callback on an IMessageObserver. Every A2 reliability class
// (Handshake, ReliableSender, SetupResponder, SessionGate) observes through this
// one seam rather than re-implementing decode/dispatch.
//
// Decoding happens inside the onFrame() call, while TinyFrame still owns the
// payload; the decoded struct is a stack local passed by const reference, so an
// observer that needs to retain any of it must copy before returning.
//
// Keep this header free of <Arduino.h> so lib/protocol stays native-compilable.
#pragma once

#include <cstddef>
#include <cstdint>

#include "frame_link.h"
#include "messages.h"

namespace protocol {

// Typed sink for decoded messages. Every method has a default no-op body so an
// observer overrides only the messages its role cares about (the CYD watches
// Ack/Nak/Telemetry; the controller watches Recipe/Start/Heartbeat; both watch
// Hello). onUnknownType/onDecodeError surface frames the router couldn't route.
struct IMessageObserver {
  virtual ~IMessageObserver() = default;

  virtual void onHello(const oven_Hello &) {}
  virtual void onRecipe(const oven_Recipe &) {}
  virtual void onStart(const oven_Start &) {}
  virtual void onHeartbeat(const oven_Heartbeat &) {}
  virtual void onAbort() {}
  virtual void onAck(const oven_Ack &) {}
  virtual void onNak(const oven_Nak &) {}
  virtual void onTelemetry(const oven_Telemetry &) {}
  virtual void onDone(const oven_Done &) {}
  virtual void onFault(const oven_Fault &) {}

  // Profile & settings management (design.md §9; added 2026-07-17). Requests are handled by
  // the controller's responder; replies by the CYD's client. Default no-ops, so a role
  // overrides only its half.
  virtual void onProfileListReq(const oven_ProfileListReq &) {}
  virtual void onProfileGetReq(const oven_ProfileGetReq &) {}
  virtual void onProfilePut(const oven_ProfilePut &) {}
  virtual void onProfileDelete(const oven_ProfileDelete &) {}
  virtual void onProfileDup(const oven_ProfileDup &) {}
  virtual void onProfileRename(const oven_ProfileRename &) {}
  virtual void onProfileTouch(const oven_ProfileTouch &) {}
  virtual void onSettingsGetReq(const oven_SettingsGetReq &) {}
  virtual void onSettingsPut(const oven_SettingsPut &) {}
  virtual void onProfileList(const oven_ProfileList &) {}
  virtual void onProfileData(const oven_ProfileData &) {}
  virtual void onSettingsData(const oven_SettingsData &) {}
  virtual void onMgmtResult(const oven_MgmtResult &) {}

  // A frame whose type id isn't in the known set (payload valid only for the
  // call, same rule as IFrameHandler).
  virtual void onUnknownType(uint8_t /*type*/, const uint8_t * /*payload*/, size_t /*len*/) {}
  // A known type whose payload failed to decode (bad CRC never reaches here —
  // TinyFrame drops those before dispatch; this is a well-framed but malformed
  // or truncated payload).
  virtual void onDecodeError(uint8_t /*type*/) {}
};

class MessageRouter : public IFrameHandler {
public:
  // observer must outlive this router.
  explicit MessageRouter(IMessageObserver &observer) : observer_(&observer) {}

  // Late-bound wiring: FrameLink binds its handler at construction and an
  // observer often needs the FrameLink (to send) before it exists, so the
  // observer can be attached after the router/link are built. Frames arriving
  // before an observer is set are dropped.
  MessageRouter() = default;
  void setObserver(IMessageObserver &observer) { observer_ = &observer; }

  void onFrame(uint8_t type, const uint8_t *payload, size_t len) override;

private:
  IMessageObserver *observer_ = nullptr;
};

} // namespace protocol
