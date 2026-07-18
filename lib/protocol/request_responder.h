// RequestResponder — the controller's half of the request/reply management path (design.md §9,
// added 2026-07-17 with the §2 "CYD is a UI remote" split). The mirror of RequestClient: it
// dedups a retransmitted request by caching the last {seq -> reply frame} and replaying it
// WITHOUT re-running the side effect, so a dropped reply (which makes the CYD resend) never
// double-applies a write.
//
// It is a reusable helper, not a full responder: a concrete responder (ManagementResponder)
// composes it. Per request the responder calls isNew(seq); if false it has already replayed the
// cached reply, so the caller just returns; if true the caller runs the store op and calls
// reply() to encode + cache + send the response. One outstanding request on the wire (RequestClient
// enforces it), so a lone last_seq_ is enough to spot a duplicate — the SetupResponder pattern.
//
// Keep free of <Arduino.h> so lib/protocol stays native-compilable.
#pragma once

#include <cstddef>
#include <cstdint>

#include "frame_link.h"
#include "oven.pb.h"

namespace protocol {

class RequestResponder {
public:
  explicit RequestResponder(FrameLink &link) : link_(link) {}

  // Forget the last-processed seq. Call when the peer reboots (its Hello carries a new
  // boot_nonce): the CYD's RequestClient re-seeds its seq per boot, so without this a re-used seq
  // would be mistaken for a retransmit and its op silently skipped (§9 re-sync).
  void reset() { have_last_ = false; }

  // Returns true for a NEW request (run the op, then call reply()); false for a duplicate — in
  // which case the cached reply has already been re-sent and the caller must do nothing else.
  bool isNew(uint32_t seq) {
    if (have_last_ && seq == last_seq_) {
      link_.send(last_type_, last_buf_, last_len_); // replay; no side effect
      return false;
    }
    return true;
  }

  // Encode `msg` as a reply of `type`, cache it (for dedup), and send it. Call once per NEW
  // request. A message too large to cache is dropped (never happens: replies fit oven_ProfileList).
  void reply(uint32_t seq, uint8_t type, const pb_msgdesc_t *fields, const void *msg);

private:
  FrameLink &link_;
  bool have_last_ = false;
  uint32_t last_seq_ = 0;
  uint8_t last_type_ = 0;
  uint8_t last_buf_[oven_ProfileList_size] = {0}; // ProfileList is the largest reply
  size_t last_len_ = 0;
};

} // namespace protocol
