// ManagementResponder — the controller's handler for the profile/settings management path
// (design.md §9; Wave R2 of the §2 "CYD is a UI remote" split, 2026-07-17). An IMessageObserver
// over the request half of that path: it answers the CYD's ProfileList/Get/Put/Delete/Dup/Rename
// requests off the two per-mode ProfileStores, deduping retransmits via a RequestResponder so a
// dropped reply never double-applies a write. Wired behind ControllerLink (setManagementObserver),
// the same seam SetupResponder occupies for the setup path.
//
// Settings requests (SettingsGet/Put) land in Wave R2b; their virtuals are left as the base's
// no-ops for now, so the CYD's settings client simply times out until then.
//
// Pure C++ over nanopb — host-tested under native_control against FakeProfileStorage.
#pragma once

#include <cstddef>
#include <cstring>

#include "codec.h" // protocol::wireEnum
#include "device_settings.h"
#include "message_router.h"
#include "messages.h"
#include "oven.pb.h"
#include "profile_library.h"
#include "request_responder.h"

class ManagementResponder : public protocol::IMessageObserver {
public:
  ManagementResponder(protocol::FrameLink &link, control::ProfileStore &cure,
                      control::ProfileStore &reflow)
      : rr_(link), cure_(cure), reflow_(reflow) {}

  // Attach the device-settings store (Wave R2b) so SettingsGet/Put are answered. Optional: without
  // it those requests return a MgmtResult error and the CYD keeps its defaults.
  void setSettingsStore(control::SettingsStore &s) { settings_ = &s; }

  // Forget the dedup cache on peer reboot (§9 re-sync) — forwarded from ControllerLink's onHello.
  void reset() { rr_.reset(); }

  void onProfileListReq(const oven_ProfileListReq &m) override {
    if (!rr_.isNew(m.seq)) {
      return;
    }
    oven_ProfileList r = oven_ProfileList_init_zero;
    r.seq = m.seq;
    if (control::ProfileStore *s = storeFor(m.mode)) {
      control::ProfileStore::Summary rows[control::ProfileStore::kMaxListed];
      size_t n = s->list(rows, control::ProfileStore::kMaxListed);
      const size_t cap = sizeof(r.profiles) / sizeof(r.profiles[0]);
      if (n > cap) {
        n = cap;
      }
      r.profiles_count = static_cast<pb_size_t>(n);
      for (size_t i = 0; i < n; ++i) {
        std::strncpy(r.profiles[i].name, rows[i].name, sizeof(r.profiles[i].name) - 1);
        r.profiles[i].stock = rows[i].stock;
        r.profiles[i].peak_c = rows[i].facts.peak_c;
        r.profiles[i].total_s = rows[i].facts.total_s;
      }
    }
    rr_.reply(m.seq, protocol::kTfTypeProfileList, oven_ProfileList_fields, &r);
  }

  void onProfileGetReq(const oven_ProfileGetReq &m) override {
    if (!rr_.isNew(m.seq)) {
      return;
    }
    control::ProfileStore *s = storeFor(m.mode);
    oven_Profile p = oven_Profile_init_zero;
    if (s != nullptr && s->load(m.name, p)) {
      oven_ProfileData r = oven_ProfileData_init_zero;
      r.seq = m.seq;
      r.has_profile = true;
      r.profile = p;
      rr_.reply(m.seq, protocol::kTfTypeProfileData, oven_ProfileData_fields, &r);
      return;
    }
    replyResult(m.seq, false, oven_NakReason_NAK_NOT_FOUND);
  }

  void onProfilePut(const oven_ProfilePut &m) override {
    if (!rr_.isNew(m.seq)) {
      return;
    }
    control::ProfileStore *s = m.has_profile ? storeFor(m.profile.mode) : nullptr;
    if (s == nullptr) {
      replyResult(m.seq, false, oven_NakReason_NAK_MODE_CONTENT_MISMATCH);
      return;
    }
    oven_NakReason reason = oven_NakReason_NAK_UNSPECIFIED;
    const bool ok = doPut(*s, m.profile, reason);
    replyResult(m.seq, ok, reason);
  }

  void onProfileDelete(const oven_ProfileDelete &m) override {
    if (!rr_.isNew(m.seq)) {
      return;
    }
    control::ProfileStore *s = storeFor(m.mode);
    if (s == nullptr) {
      replyResult(m.seq, false, oven_NakReason_NAK_MODE_CONTENT_MISMATCH);
      return;
    }
    if (s->isStock(m.name)) {
      replyResult(m.seq, false, oven_NakReason_NAK_STOCK_READONLY);
      return;
    }
    if (!s->contains(m.name)) {
      replyResult(m.seq, false, oven_NakReason_NAK_NOT_FOUND);
      return;
    }
    replyResult(m.seq, s->remove(m.name), oven_NakReason_NAK_OUT_OF_RANGE);
  }

  void onProfileDup(const oven_ProfileDup &m) override {
    if (!rr_.isNew(m.seq)) {
      return;
    }
    control::ProfileStore *s = storeFor(m.mode);
    if (s == nullptr) {
      replyResult(m.seq, false, oven_NakReason_NAK_MODE_CONTENT_MISMATCH);
      return;
    }
    if (!s->contains(m.src)) {
      replyResult(m.seq, false, oven_NakReason_NAK_NOT_FOUND);
      return;
    }
    if (!control::ProfileStore::validName(m.dst) || s->contains(m.dst)) {
      replyResult(m.seq, false, oven_NakReason_NAK_NAME_INVALID);
      return;
    }
    replyResult(m.seq, s->duplicate(m.src, m.dst), oven_NakReason_NAK_OUT_OF_RANGE);
  }

  void onProfileRename(const oven_ProfileRename &m) override {
    if (!rr_.isNew(m.seq)) {
      return;
    }
    control::ProfileStore *s = storeFor(m.mode);
    if (s == nullptr) {
      replyResult(m.seq, false, oven_NakReason_NAK_MODE_CONTENT_MISMATCH);
      return;
    }
    if (!s->contains(m.old_name)) {
      replyResult(m.seq, false, oven_NakReason_NAK_NOT_FOUND);
      return;
    }
    if (s->isStock(m.old_name)) {
      replyResult(m.seq, false, oven_NakReason_NAK_STOCK_READONLY);
      return;
    }
    const bool same = std::strcmp(m.old_name, m.new_name) == 0;
    if (!control::ProfileStore::validName(m.new_name) || (!same && s->contains(m.new_name))) {
      replyResult(m.seq, false, oven_NakReason_NAK_NAME_INVALID);
      return;
    }
    replyResult(m.seq, s->rename(m.old_name, m.new_name), oven_NakReason_NAK_OUT_OF_RANGE);
  }

  void onSettingsGetReq(const oven_SettingsGetReq &m) override {
    if (!rr_.isNew(m.seq)) {
      return;
    }
    if (settings_ == nullptr) {
      replyResult(m.seq, false, oven_NakReason_NAK_UNSPECIFIED);
      return;
    }
    oven_SettingsData r = oven_SettingsData_init_zero;
    r.seq = m.seq;
    r.has_settings = true;
    r.settings = settings_->get();
    rr_.reply(m.seq, protocol::kTfTypeSettingsData, oven_SettingsData_fields, &r);
  }

  void onSettingsPut(const oven_SettingsPut &m) override {
    if (!rr_.isNew(m.seq)) {
      return;
    }
    if (settings_ == nullptr || !m.has_settings) {
      replyResult(m.seq, false, oven_NakReason_NAK_UNSPECIFIED);
      return;
    }
    replyResult(m.seq, settings_->save(m.settings), oven_NakReason_NAK_OUT_OF_RANGE);
  }

private:
  // Untrusted mode tag -> the matching per-mode store (nullptr for UNSPECIFIED / out-of-range).
  control::ProfileStore *storeFor(const oven_Mode &mode) {
    switch (protocol::wireEnum(mode)) {
    case oven_Mode_MODE_CURE:
      return &cure_;
    case oven_Mode_MODE_REFLOW:
      return &reflow_;
    default:
      return nullptr;
    }
  }

  // Save with a mapped NAK reason. The store stamps its own mode, so a cross-mode profile is
  // filed correctly rather than rejected — storeFor already picked the dir from the tag.
  static bool doPut(control::ProfileStore &s, const oven_Profile &p, oven_NakReason &reason) {
    if (!control::ProfileStore::validName(p.name)) {
      reason = oven_NakReason_NAK_NAME_INVALID;
      return false;
    }
    if (s.isStock(p.name)) {
      reason = oven_NakReason_NAK_STOCK_READONLY;
      return false;
    }
    if (p.phases_count == 0 || p.phases_count > control::ProfileStore::kMaxPhases) {
      reason = oven_NakReason_NAK_OUT_OF_RANGE;
      return false;
    }
    if (!s.save(p)) {
      reason = oven_NakReason_NAK_OUT_OF_RANGE;
      return false;
    }
    return true;
  }

  void replyResult(uint32_t seq, bool ok, oven_NakReason reason) {
    oven_MgmtResult r = oven_MgmtResult_init_zero;
    r.seq = seq;
    r.ok = ok;
    r.reason = ok ? oven_NakReason_NAK_UNSPECIFIED : reason;
    rr_.reply(seq, protocol::kTfTypeMgmtResult, oven_MgmtResult_fields, &r);
  }

  protocol::RequestResponder rr_;
  control::ProfileStore &cure_;
  control::ProfileStore &reflow_;
  control::SettingsStore *settings_ = nullptr;
};
