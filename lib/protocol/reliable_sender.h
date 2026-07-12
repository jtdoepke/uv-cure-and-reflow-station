// ReliableSender — the CYD's exactly-once setup path (design.md §9).
//
// Recipe upload and Start change persistent controller state, so they can't ride
// the self-healing hot path; they must land. Each carries a monotonic seq (added
// to those messages for exactly this) that the controller echoes in Ack{seq} /
// Nak{seq}. This sender stamps the seq, keeps the encoded frame, and resends it
// every kSetupAckTimeoutMs until an Ack arrives, up to kSetupMaxRetries resends;
// after that it reports Failed for the UI to surface. A Nak reports Nakd with the
// reason (out-of-range, mode/content mismatch, …).
//
// Single outstanding command: send Recipe, await Ack, then send Start. A new
// send is refused while one is Pending. Reads the clock internally in service().
// Keep free of <Arduino.h>.
#pragma once

#include <cstddef>
#include <cstdint>

#include "IClock.h"
#include "frame_link.h"
#include "messages.h"

namespace protocol {

class ReliableSender {
public:
  enum class State { Idle, Pending, Acked, Nakd, Failed };

  // link and clock must outlive this sender.
  ReliableSender(FrameLink &link, IClock &clock) : link_(link), clock_(clock) {}

  // Stamp the next seq into a copy of recipe/start, send it, and enter Pending.
  // Returns false (sending nothing) if a command is already outstanding.
  bool sendRecipe(const oven_Recipe &recipe);
  bool sendStart(const oven_Start &start);

  // Resend the outstanding command on timeout; give up (Failed) after the retry
  // budget. Call every loop iteration; a no-op unless Pending.
  void service();

  // Feed decoded acknowledgements (from MessageRouter). A seq mismatch is
  // ignored (a stale/duplicate ack for an already-resolved command).
  void onAck(const oven_Ack &ack);
  void onNak(const oven_Nak &nak);

  State state() const { return state_; }
  bool busy() const { return state_ == State::Pending; }
  uint32_t pendingSeq() const { return pending_seq_; }
  oven_NakReason lastNakReason() const { return last_nak_reason_; }
  // Resends performed for the current/most-recent command (excludes the initial
  // send); handy for tests and diagnostics.
  uint8_t attempts() const { return attempts_; }

private:
  bool begin(uint8_t type, const uint8_t *payload, size_t len);
  void resend();

  FrameLink &link_;
  IClock &clock_;

  State state_ = State::Idle;
  uint32_t seq_ = 0;         // monotonic; ++seq_ assigned per command
  uint32_t pending_seq_ = 0; // seq of the outstanding command
  uint8_t attempts_ = 0;     // resends so far for the outstanding command
  uint32_t last_send_ms_ = 0;
  oven_NakReason last_nak_reason_ = oven_NakReason_NAK_UNSPECIFIED;

  // Cached framed payload of the outstanding command, replayed verbatim on
  // retry (same seq, same bytes). Recipe is the larger of the two.
  uint8_t type_ = 0;
  uint8_t buf_[oven_Recipe_size] = {0};
  size_t len_ = 0;
};

} // namespace protocol
