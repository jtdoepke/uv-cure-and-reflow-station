// RequestClient — the CYD's correlated request/reply path (design.md §9 "Profile &
// settings management", added 2026-07-17 with the §2 "CYD is a UI remote" split).
//
// A third reliability shape, alongside the hot path (Heartbeat/Telemetry, fire-and-forget)
// and the setup path (Recipe/Start, request->verdict). This one is request->DATA-reply: the
// CYD asks the controller to list/read/write a profile or get/put settings, and the reply
// carries a payload (a ProfileList, a Profile, a Settings, or a MgmtResult verdict), not just
// an Ack. Same discipline as the setup path — a monotonic seq, ONE outstanding request,
// resend on timeout, deduped so a resent request never double-applies — but generic over the
// reply type: the client owns only retry + seq-correlation; the app decodes the reply and
// tells us the seq is resolved via onReply().
//
// Kept a SEPARATE class (and separate seq counter) from ReliableSender on purpose: management
// traffic is idle-context UI chatter and must never share state with, or perturb, the
// authorization-bearing setup/hot paths the SessionGate/SafetySupervisor depend on. The caller
// stamps nextSeq() into the request message and encodes it, then hands the framed bytes here —
// so this class needs no knowledge of any message's layout.
//
// Keep free of <Arduino.h> so lib/protocol stays native-compilable.
#pragma once

#include <cstddef>
#include <cstdint>

#include "IClock.h"
#include "frame_link.h"
#include "messages.h"
#include "oven.pb.h"

namespace protocol {

class RequestClient {
public:
  enum class State { Idle, Pending, Done, Failed };

  // link and clock must outlive this client.
  RequestClient(FrameLink &link, IClock &clock) : link_(link), clock_(clock) {}

  // Seed the seq counter; the next request uses base + 1. Seed from a per-boot random source
  // at boot (before the first request) for the same reason ReliableSender does: the
  // controller's responder dedups on seq, so a rebooted CYD restarting at 0 could have its
  // first request mistaken for a replay. Unseeded, it starts at 0 (correct for tests).
  void setSeqBase(uint32_t base) { seq_ = base; }

  // Allocate the seq to stamp into the next request message before encoding it. Only advances
  // the counter; the request is not outstanding until send().
  uint32_t nextSeq() { return ++seq_; }

  // Send an already-encoded request of `type` carrying `payload` (which must contain `seq`,
  // typically from nextSeq()). Caches the framed bytes for retransmit and enters Pending.
  // Returns false (sending nothing) if a request is already outstanding (Pending) or the
  // payload is too large to cache.
  bool send(uint8_t type, const uint8_t *payload, size_t len, uint32_t seq);

  // Resend the outstanding request on timeout; give up (Failed) after the retry budget. Call
  // every loop iteration; a no-op unless Pending.
  void service();

  // The app calls this when a reply (ProfileList / ProfileData / SettingsData / MgmtResult)
  // with matching seq arrives — the client only correlates; the app owns the payload. A seq
  // mismatch is ignored (a stale/duplicate reply for an already-resolved request).
  void onReply(uint32_t seq);

  // Return to Idle after consuming a terminal (Done/Failed) result, freeing the client for the
  // next request. A no-op while Pending, so an accidental call can't drop an in-flight request.
  void clear();

  State state() const { return state_; }
  bool busy() const { return state_ == State::Pending; }
  bool idle() const { return state_ == State::Idle; }
  uint32_t pendingSeq() const { return pending_seq_; }
  uint8_t attempts() const { return attempts_; }

private:
  void resend();

  FrameLink &link_;
  IClock &clock_;

  State state_ = State::Idle;
  uint32_t seq_ = 0;         // monotonic; nextSeq() returns ++seq_
  uint32_t pending_seq_ = 0; // seq of the outstanding request
  uint8_t attempts_ = 0;     // resends so far for the outstanding request
  uint32_t last_send_ms_ = 0;

  // Cached framed payload of the outstanding request, replayed verbatim on retry (same seq,
  // same bytes). ProfilePut (a full 32-phase Profile) is the largest request.
  uint8_t type_ = 0;
  uint8_t buf_[oven_ProfilePut_size] = {0};
  size_t len_ = 0;
};

} // namespace protocol
