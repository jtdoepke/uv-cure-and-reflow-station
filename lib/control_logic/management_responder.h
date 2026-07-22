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
#include "stock_seed.h" // §24 Restore stock profiles — the compiled-in factory set

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
    // The reply message (~1.5 KB) and the list scratch (~1.4 KB) are members, not stack locals:
    // this runs several frames deep behind the FrameLink poll on the safety MCU's loopTask, and
    // together they are the dominant frame on the list path. Reuse is safe — the responder handles
    // one request at a time (RequestResponder enforces single-outstanding) on the single control
    // loop, and rr_.reply() copies the encoded bytes out before returning.
    oven_ProfileList &r = reply_.list;
    r = oven_ProfileList_init_zero;
    r.seq = m.seq;
    if (control::ProfileStore *s = storeFor(m.mode)) {
      const control::ProfileStore::SortMode sort =
          protocol::wireEnum(m.sort) == oven_ProfileSort_PROFILE_SORT_MRU
              ? control::ProfileStore::SortMode::Mru
              : control::ProfileStore::SortMode::Alpha;
      size_t n = s->list(rows_, control::ProfileStore::kMaxListed, sort);
      const size_t cap = sizeof(r.profiles) / sizeof(r.profiles[0]);
      if (n > cap) {
        n = cap;
      }
      r.profiles_count = static_cast<pb_size_t>(n);
      for (size_t i = 0; i < n; ++i) {
        std::strncpy(r.profiles[i].name, rows_[i].name, sizeof(r.profiles[i].name) - 1);
        r.profiles[i].stock = rows_[i].stock;
        r.profiles[i].peak_c = rows_[i].facts.peak_c;
        r.profiles[i].total_s = rows_[i].facts.total_s;
      }
    }
    rr_.reply(m.seq, protocol::kTfTypeProfileList, oven_ProfileList_fields, &r);
  }

  void onProfileGetReq(const oven_ProfileGetReq &m) override {
    if (!rr_.isNew(m.seq)) {
      return;
    }
    control::ProfileStore *s = storeFor(m.mode);
    oven_ProfileData &r = reply_.data; // shares storage with reply_.list — never both live
    r = oven_ProfileData_init_zero;
    if (s != nullptr && s->load(m.name, r.profile)) { // decode straight into the reply; no 2nd copy
      r.seq = m.seq;
      r.has_profile = true;
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

  // §23/§24 "Restore stock profiles": reinstall this mode's stock set from the firmware's own
  // compiled-in table. The request carries no profile content by design (see oven.proto) — the CYD
  // triggers the privileged write, the controller alone decides what gets written.
  //
  // A restore that finds a USER profile squatting a stock name reports failure rather than
  // clobbering it, because silently overwriting the operator's work to reinstate a factory
  // reference is the wrong trade — and a silent success would be worse, since they would never
  // learn why the stock profile is still missing.
  void onProfileRestoreStock(const oven_ProfileRestoreStock &m) override {
    if (!rr_.isNew(m.seq)) {
      return;
    }
    control::ProfileStore *s = storeFor(m.mode);
    if (s == nullptr) {
      replyResult(m.seq, false, oven_NakReason_NAK_MODE_CONTENT_MISMATCH);
      return;
    }
    const control::SeedReport r = control::seedStockProfiles(*s, /*overwrite=*/true);
    if (r.considered() == 0) {
      // This mode has no entries in the compiled table at all — nothing to restore, rather than a
      // restore that succeeded. Reporting ok here would tell the operator their library was
      // reinstated when it was not touched, which is the kind of quiet lie a maintenance action
      // must never tell.
      replyResult(m.seq, false, oven_NakReason_NAK_NOT_FOUND);
      return;
    }
    if (r.userOwned > 0) {
      replyResult(m.seq, false, oven_NakReason_NAK_NAME_INVALID); // the name is taken by a user
      return;
    }
    replyResult(m.seq, r.ok(), oven_NakReason_NAK_OUT_OF_RANGE);
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

  void onProfileTouch(const oven_ProfileTouch &m) override {
    if (!rr_.isNew(m.seq)) {
      return;
    }
    control::ProfileStore *s = storeFor(m.mode);
    if (s == nullptr) {
      replyResult(m.seq, false, oven_NakReason_NAK_MODE_CONTENT_MISMATCH);
      return;
    }
    // touch() bumps recency (allowed on stock — running a stock profile is a use); false only if
    // the profile is absent. The CYD ignores this verdict — a lost touch just doesn't reorder.
    replyResult(m.seq, s->touch(m.name), oven_NakReason_NAK_NOT_FOUND);
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

  // Reusable reply scratch kept off the loopTask stack (see onProfileListReq). A union: only one
  // reply is built at a time, and ProfileList / ProfileData are never both live — the same trick
  // ManagementClient uses on the CYD. SettingsData/MgmtResult are tiny and stay on the stack. POD
  // nanopb structs, so the union needs no active-member bookkeeping. ~1.5 KB static on the
  // controller, which has ample internal DRAM; it buys ~3 KB of loopTask stack headroom with rows_.
  union ReplyScratch {
    oven_ProfileList list;
    oven_ProfileData data;
  } reply_{};
  control::ProfileStore::Summary rows_[control::ProfileStore::kMaxListed]{};
};
