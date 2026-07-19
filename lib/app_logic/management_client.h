// ManagementClient — the CYD's remote client for the profile library + device settings (design.md
// §9; Wave R3 of the §2 "CYD is a UI remote" split, 2026-07-17). The mirror of the controller's
// ManagementResponder: the profile store + settings now live on the controller, so the CYD's
// library/editor/settings screens (§12/§23/§24) drive them through this client over the link.
//
// It wraps one protocol::RequestClient (single-outstanding, seq-correlated, retry-until-reply) and
// is the IMessageObserver for the four reply types. An operation returns false if one is already in
// flight; otherwise the screen polls state() until it leaves Busy — Ready (a reply arrived, payload
// in list()/profile()/settings()) or Failed (a NAK, or the retry budget ran out) — then reads the
// result and clear()s for the next one. That async Busy/Ready/Failed cycle is what a local flash
// store never needed (the screens gain a loading/error state, §23/§24).
//
// Pure C++ over nanopb — no LVGL, no Arduino — host-tested under native_logic_cyd / a full-stack
// round-trip against the real ManagementResponder (native_control).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "IClock.h"
#include "codec.h"
#include "frame_link.h"
#include "message_router.h" // IMessageObserver
#include "messages.h"
#include "oven.pb.h"
#include "request_client.h"

class ManagementClient : public protocol::IMessageObserver {
public:
  enum class Op : uint8_t {
    None,
    List,
    Get,
    Put,
    Delete,
    Dup,
    Rename,
    Touch,
    SettingsGet,
    SettingsPut
  };
  enum class State : uint8_t { Idle, Busy, Ready, Failed };

  ManagementClient(protocol::FrameLink &link, IClock &clock) : rc_(link, clock) {}

  // Seed the seq counter per boot (see RequestClient::setSeqBase).
  void setSeqBase(uint32_t base) { rc_.setSeqBase(base); }

  // Drive retry; promote a timed-out request to Failed. Call every loop iteration.
  void service() {
    rc_.service();
    if (state_ == State::Busy && rc_.state() == protocol::RequestClient::State::Failed) {
      state_ = State::Failed;
      nak_ = oven_NakReason_NAK_UNSPECIFIED; // no reply at all
      rc_.clear();
    }
  }

  // --- Operations (return false if one is already outstanding) ---
  bool requestList(oven_Mode mode, oven_ProfileSort sort = oven_ProfileSort_PROFILE_SORT_ALPHA) {
    oven_ProfileListReq m = oven_ProfileListReq_init_zero;
    m.mode = mode;
    m.sort = sort;
    return begin(Op::List, protocol::kTfTypeProfileListReq, oven_ProfileListReq_fields, &m);
  }
  bool requestGet(oven_Mode mode, const char *name) {
    oven_ProfileGetReq m = oven_ProfileGetReq_init_zero;
    m.mode = mode;
    copyName(m.name, sizeof(m.name), name);
    return begin(Op::Get, protocol::kTfTypeProfileGetReq, oven_ProfileGetReq_fields, &m);
  }
  bool requestPut(const oven_Profile &p) {
    oven_ProfilePut m = oven_ProfilePut_init_zero;
    m.has_profile = true;
    m.profile = p;
    return begin(Op::Put, protocol::kTfTypeProfilePut, oven_ProfilePut_fields, &m);
  }
  bool requestDelete(oven_Mode mode, const char *name) {
    oven_ProfileDelete m = oven_ProfileDelete_init_zero;
    m.mode = mode;
    copyName(m.name, sizeof(m.name), name);
    return begin(Op::Delete, protocol::kTfTypeProfileDelete, oven_ProfileDelete_fields, &m);
  }
  bool requestDup(oven_Mode mode, const char *src, const char *dst) {
    oven_ProfileDup m = oven_ProfileDup_init_zero;
    m.mode = mode;
    copyName(m.src, sizeof(m.src), src);
    copyName(m.dst, sizeof(m.dst), dst);
    return begin(Op::Dup, protocol::kTfTypeProfileDup, oven_ProfileDup_fields, &m);
  }
  bool requestRename(oven_Mode mode, const char *oldName, const char *newName) {
    oven_ProfileRename m = oven_ProfileRename_init_zero;
    m.mode = mode;
    copyName(m.old_name, sizeof(m.old_name), oldName);
    copyName(m.new_name, sizeof(m.new_name), newName);
    return begin(Op::Rename, protocol::kTfTypeProfileRename, oven_ProfileRename_fields, &m);
  }
  // Mark a profile "used" for the MRU sort (§23). Fired at run-start; the caller ignores the
  // MgmtResult verdict (a lost touch is merely cosmetic), so it just needs the request slot free.
  bool requestTouch(oven_Mode mode, const char *name) {
    oven_ProfileTouch m = oven_ProfileTouch_init_zero;
    m.mode = mode;
    copyName(m.name, sizeof(m.name), name);
    return begin(Op::Touch, protocol::kTfTypeProfileTouch, oven_ProfileTouch_fields, &m);
  }
  bool requestSettingsGet() {
    oven_SettingsGetReq m = oven_SettingsGetReq_init_zero;
    return begin(Op::SettingsGet, protocol::kTfTypeSettingsGetReq, oven_SettingsGetReq_fields, &m);
  }
  bool requestSettingsPut(const oven_Settings &s) {
    oven_SettingsPut m = oven_SettingsPut_init_zero;
    m.has_settings = true;
    m.settings = s;
    return begin(Op::SettingsPut, protocol::kTfTypeSettingsPut, oven_SettingsPut_fields, &m);
  }

  // --- Result inspection ---
  State state() const { return state_; }
  Op lastOp() const { return op_; }
  bool idle() const { return state_ == State::Idle; }
  bool busy() const { return state_ == State::Busy; }
  bool ready() const { return state_ == State::Ready; }
  bool failed() const { return state_ == State::Failed; }
  const oven_ProfileList &list() const { return reply_.list; }   // valid after a List
  const oven_Profile &profile() const { return reply_.profile; } // valid after a Get
  const oven_Settings &settings() const { return settings_; }    // valid after a SettingsGet
  oven_NakReason lastNak() const { return nak_; }

  // Ack a terminal result, returning to Idle so the next request can go. No-op while Busy.
  void clear() {
    if (state_ != State::Busy) {
      state_ = State::Idle;
      op_ = Op::None;
      rc_.clear();
    }
  }

  // --- IMessageObserver (reply types; forwarded by CydLink) ---
  void onProfileList(const oven_ProfileList &m) override {
    if (rc_.busy() && m.seq == rc_.pendingSeq()) {
      reply_.list = m;
      state_ = State::Ready;
      rc_.onReply(m.seq);
    }
  }
  void onProfileData(const oven_ProfileData &m) override {
    if (rc_.busy() && m.seq == rc_.pendingSeq()) {
      if (m.has_profile) {
        reply_.profile = m.profile;
      }
      state_ = State::Ready;
      rc_.onReply(m.seq);
    }
  }
  void onSettingsData(const oven_SettingsData &m) override {
    if (rc_.busy() && m.seq == rc_.pendingSeq()) {
      if (m.has_settings) {
        settings_ = m.settings;
      }
      state_ = State::Ready;
      rc_.onReply(m.seq);
    }
  }
  // MgmtResult is the verdict for a mutation AND the negative reply for a Get/List/SettingsGet
  // (e.g. NAK_NOT_FOUND), so it applies whatever the outstanding op was.
  void onMgmtResult(const oven_MgmtResult &m) override {
    if (rc_.busy() && m.seq == rc_.pendingSeq()) {
      nak_ = protocol::wireEnumOr(m.reason, _oven_NakReason_MIN, _oven_NakReason_MAX,
                                  oven_NakReason_NAK_UNSPECIFIED);
      state_ = m.ok ? State::Ready : State::Failed;
      rc_.onReply(m.seq);
    }
  }

private:
  template <typename Msg> bool begin(Op op, uint8_t type, const pb_msgdesc_t *fields, Msg *m) {
    if (rc_.busy()) {
      return false;
    }
    const uint32_t seq = rc_.nextSeq();
    m->seq = seq;
    uint8_t buf[oven_ProfilePut_size]; // ProfilePut is the largest request
    size_t len = 0;
    if (!protocol::encode(fields, m, buf, sizeof(buf), len)) {
      return false;
    }
    if (!rc_.send(type, buf, len, seq)) {
      return false;
    }
    op_ = op;
    state_ = State::Busy;
    nak_ = oven_NakReason_NAK_UNSPECIFIED;
    return true;
  }

  static void copyName(char *dst, size_t cap, const char *src) {
    std::strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
  }

  protocol::RequestClient rc_;
  Op op_ = Op::None;
  State state_ = State::Idle;
  // list and profile are never both valid (a List reply vs a Get reply), so they share storage to
  // save ~1.5 KB of the scarce CYD DRAM (Wave R3b). The accessors return the one the last op set;
  // reading the other would be a mismatched op the caller never does. settings is separate (tiny).
  union Reply {
    oven_ProfileList list;
    oven_Profile profile;
  } reply_ = {};
  oven_Settings settings_ = oven_Settings_init_zero;
  oven_NakReason nak_ = oven_NakReason_NAK_UNSPECIFIED;
};
