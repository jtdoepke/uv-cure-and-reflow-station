// profile_library_viewmodel.h — the per-mode logic behind the §23 profile library (backlog C4;
// rewired for Wave R3b of the §2 "CYD is a UI remote" split, 2026-07-17).
//
// The profile store moved to the controller, so this view-model is now a REMOTE client's cache +
// request issuer over a shared ManagementClient (§9), not a proxy over a local ProfileStore. It is
// async: an operation issues a request and returns; the screen polls the client and calls the
// adopt*() method when the reply lands. Two caches back the synchronous read interface the screen
// renders from:
//   - the LIST cache (ProfileSummary rows: name, stock, and the peak/duration facts the CONTROLLER
//     computed, so a row needs no per-profile fetch), and
//   - the DETAIL cache (the selected profile's phases, decoded via phase_codec after a Get), off
//     which the curve/facts are computed locally with the §12 math (unchanged).
//
// Pure C++ — no LVGL, no Arduino. Host-tested under native_logic_cyd / a screen-level integration
// against a real ManagementClient + fake responder.
#pragma once

#include <cstddef>
#include <cstdio>
#include <cstring>

#include "IProfileStorage.h" // kProfileNameCap (the profile-name cap, shared with oven.options)
#include "management_client.h"
#include "oven.pb.h"
#include "oven_cal.h"
#include "phase.h"
#include "phase_codec.h"
#include "profile_draft.h" // ProfileDraft — the run working copy the Setup picker hands back (C6)
#include "profile_facts.h"

class ProfileLibraryViewModel {
public:
  // Rows held at once — the paging WINDOW (== oven.ProfileList.profiles max_count ==
  // ProfileStore::kListWindow), NOT the library size. Since 2026-07-22 the list is paged (§23):
  // the controller holds up to kMaxListed (64) profiles and sorts all of them, and the CYD caches
  // one window at a time, so this buffer no longer grows when the library cap does.
  static constexpr size_t kMaxRows = 16;
  // The cache must hold whatever one reply can carry, or adoptList() would silently drop the tail
  // of a window and the operator would page past rows that never appeared. Ties the CYD's buffer to
  // the wire bound so raising oven.ProfileList.profiles without raising this fails to compile.
  static_assert(kMaxRows >= sizeof(oven_ProfileList::profiles) / sizeof(oven_ProfileSummary),
                "kMaxRows must hold a full ProfileList window");
  static constexpr size_t kValueCap = 24; // "peak 245° · ~6:10" + NUL, with °/· as 2 bytes each

  ProfileLibraryViewModel() = default;

  // `client` is the shared remote client (single-outstanding); `mode` scopes every request;
  // `model` supplies the calibration the facts/curve are computed against. All must outlive this.
  void init(ManagementClient &client, oven_Mode mode, const OvenModel &model) {
    client_ = &client;
    mode_ = mode;
    model_ = &model;
    fahrenheit_ = false;
    mru_sort_ = false;
    count_ = 0;
    offset_ = 0;
    total_ = 0;
    detail_count_ = 0;
    have_detail_ = false;
  }

  oven_Mode mode() const { return mode_; }
  RecipeMode recipeMode() const { return phase_codec::modeFromWire(mode_); }

  void setFahrenheit(bool f) { fahrenheit_ = f; }
  bool fahrenheit() const { return fahrenheit_; }

  // Row ordering (§23): false = alphabetical (manage-library base), true = most-recently-used
  // (Setup → Load picker default). The CONTROLLER sorts — it owns the recency counter and the DRAM
  // (§2/§6a) — so changing this only sets which order the NEXT requestList() asks for; the screen
  // must re-list to see it (the picker does on toggle). No CYD-side re-sort, no recency key stored.
  void setMruSort(bool on) { mru_sort_ = on; }
  bool mruSort() const { return mru_sort_; }
  // Flip the order for the picker's sort button; the caller then re-lists. Returns the new value.
  bool toggleSort() {
    mru_sort_ = !mru_sort_;
    return mru_sort_;
  }

  // --- List: request + adopt ---

  // Ask the controller for the FIRST window of this mode's library, in the current sort order
  // (§23). Returns false if the client is busy. The screen shows a loading state, polls the client,
  // and calls adoptList() when it is Ready.
  bool requestList() { return requestListAt(0); }

  // Ask for the window starting at `offset`. The controller clamps it and reports what it used, so
  // an offset that outran a shrinking library converges instead of failing.
  bool requestListAt(size_t offset) {
    return client_ != nullptr && client_->requestList(mode_, sortWire(), offset, nullptr);
  }

  // Ask for whichever window CONTAINS `name` — the refresh a mutation should use. The CYD never
  // computes a page number: after a delete anchor on the neighbour row, after a save/dup/rename on
  // the new name, after a run-start touch (which reorders MRU) on the profile that ran. A name that
  // no longer exists falls back to the current offset rather than erroring.
  bool requestListAnchored(const char *name) {
    return client_ != nullptr && client_->requestList(mode_, sortWire(), offset_, name);
  }

  // Adopt a ProfileList reply into the row cache (call from the screen's poll on Ready).
  void adoptList(const oven_ProfileList &list) {
    count_ = list.profiles_count;
    if (count_ > kMaxRows) {
      count_ = kMaxRows;
    }
    // The controller's offset, not the one we asked for: it clamps, and it resolves anchors.
    offset_ = list.offset;
    total_ = list.total;
    for (size_t i = 0; i < count_; ++i) {
      std::strncpy(name_buf_[i], list.profiles[i].name, kProfileNameCap - 1);
      name_buf_[i][kProfileNameCap - 1] = '\0';
      stock_[i] = list.profiles[i].stock;
      peak_[i] = list.profiles[i].peak_c;
      total_s_[i] = list.profiles[i].total_s;
      buildValue(i);
    }
    // Rows arrive already ordered per the request's sort (the controller sorts); render as
    // received.
  }

  // Rows in THIS window. Every index-taking method below is window-relative.
  size_t count() const { return count_; }

  // --- Paging position (§23) ---

  size_t offset() const { return offset_; } // global index of row 0 of this window
  size_t total() const { return total_; }   // profiles in the whole library
  bool morePrev() const { return offset_ > 0; }
  bool moreNext() const { return offset_ + count_ < total_; }

  // Load the adjacent window. `dir` -1 = the one above, +1 = the one below. The new window abuts
  // this one, so scrolling reads as continuous rather than jumping.
  bool requestAdjacent(int dir) {
    if (dir < 0) {
      return morePrev() && requestListAt(offset_ >= kMaxRows ? offset_ - kMaxRows : 0);
    }
    return moreNext() && requestListAt(offset_ + count_);
  }

  // Window-relative index of `name`, or -1 if it is not in this window. Lets the screen restore the
  // highlight onto the row it acted on after an anchored refresh.
  int indexOfName(const char *name) const {
    for (size_t i = 0; i < count_; ++i) {
      if (std::strcmp(name_buf_[i], name) == 0) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }
  const char *rowLabel(size_t i) const { return i < count_ ? name_buf_[i] : ""; }
  const char *rowValue(size_t i) const { return i < count_ ? value_buf_[i] : ""; }
  const char *name(size_t i) const { return rowLabel(i); }
  bool rowStock(size_t i) const { return i < count_ && stock_[i]; }
  bool canDelete(size_t i) const { return i < count_ && !stock_[i]; }
  bool editIsSaveAs(size_t i) const { return rowStock(i); }

  // Is `name` already in the CURRENT WINDOW? Lets the rename UI reject an obvious clash client-side
  // (staying on the keyboard) rather than round-tripping for a NAK (§23).
  //
  // Since the list was paged this is a one-way test: true means definitely taken, false only means
  // "not on this page". The controller has always been the authority (it NAKs a clash with
  // NAK_NAME_INVALID) — what changed is that this can no longer *prove* a name is free, so it must
  // stay a fast-path rejection and never a precondition.
  bool nameExists(const char *name) const { return nameTaken(name); }

  // --- Detail: request + adopt + derived facts/curve (off the fetched phases) ---

  // Fetch the full profile at row `i` for the detail curve. Returns false if `i` is out of range or
  // the client is busy. The screen shows loading, polls, and calls adoptDetail() on Ready.
  bool requestDetail(size_t i) {
    if (i >= count_ || client_ == nullptr) {
      return false;
    }
    return client_->requestGet(mode_, name_buf_[i]);
  }

  // Adopt a ProfileData reply — decode its phases into the detail cache (call on Ready).
  void adoptDetail(const oven_Profile &p) {
    detail_count_ = phase_codec::phasesFromWire(p, detail_phases_, kMaxPhases);
    have_detail_ = true;
  }
  bool haveDetail() const { return have_detail_; }
  void clearDetail() { have_detail_ = false; }

  size_t phaseCount() const { return detail_count_; }

  // Assemble the run working copy the Setup picker hands back (C6): the selected row's name/stock +
  // this mode + the decoded detail phases. Requires haveDetail() (the caller fetches the detail for
  // the preview first, then picks). Fills a valid empty draft if the row is out of range.
  void detailToDraft(size_t row, ProfileDraft &out) const {
    out = ProfileDraft{};
    out.mode = recipeMode();
    if (row < count_) {
      std::strncpy(out.name, name_buf_[row], kProfileNameCap - 1);
      out.name[kProfileNameCap - 1] = '\0';
      out.stock = stock_[row];
    }
    out.phaseCount = detail_count_ <= kMaxPhases ? detail_count_ : kMaxPhases;
    for (size_t i = 0; i < out.phaseCount; ++i) {
      out.phases[i] = detail_phases_[i];
    }
  }

  profile_facts::ProfileFacts facts() const {
    if (!have_detail_) {
      return {};
    }
    return profile_facts::computeFacts(detail_phases_, detail_count_, recipeMode(), *model_);
  }

  size_t phaseNames(char (*out)[kPhaseNameCap], size_t cap) const {
    size_t n = detail_count_ < cap ? detail_count_ : cap;
    for (size_t k = 0; k < n; ++k) {
      std::strncpy(out[k], detail_phases_[k].name, kPhaseNameCap - 1);
      out[k][kPhaseNameCap - 1] = '\0';
    }
    return n;
  }

  size_t sampleRequested(profile_facts::CurvePoint *out, size_t cap) const {
    return have_detail_ ? profile_facts::sampleCurve(detail_phases_, detail_count_, recipeMode(),
                                                     *model_, /*achievable=*/false,
                                                     profile_facts::kDefaultAmbientC, out, cap)
                        : 0;
  }
  size_t sampleOvershoot(profile_facts::CurvePoint *out, size_t cap) const {
    return have_detail_
               ? profile_facts::sampleOvershoot(detail_phases_, detail_count_, recipeMode(),
                                                *model_, profile_facts::kDefaultAmbientC, out, cap)
               : 0;
  }
  size_t samplePhaseBoundaries(float *out, size_t cap) const {
    return have_detail_
               ? profile_facts::samplePhaseBoundaries(detail_phases_, detail_count_, recipeMode(),
                                                      *model_, profile_facts::kDefaultAmbientC, out,
                                                      cap)
               : 0;
  }
  size_t sampleUvSpans(profile_facts::TimeSpan *out, size_t cap) const {
    return have_detail_
               ? profile_facts::sampleUvSpans(detail_phases_, detail_count_, recipeMode(), *model_,
                                              profile_facts::kDefaultAmbientC, out, cap)
               : 0;
  }
  bool uncalibrated() const { return !model_->calibrated; }

  // --- Actions: issue a request; the screen polls and re-lists on Ready ---

  // Duplicate row `i` as "<name> copy" (or " copy 2", …), deconflicting against the current WINDOW
  // (the controller refuses a clash authoritatively, §23). Returns false if `i` is out of range, no
  // free name was found, or the client is busy.
  //
  // Paging narrowed what this can see, but barely, and only here: Dup exists solely in the manage
  // context, whose sort is alphabetical (§23) — so "X", "X copy" and "X copy 2" sort adjacently and
  // land in the same window except when X is the last row of one. In that case the controller NAKs
  // and the screen surfaces the error, which is the pre-existing behaviour for a raced clash.
  bool requestDuplicate(size_t i) {
    if (i >= count_ || client_ == nullptr) {
      return false;
    }
    char dst[kProfileNameCap];
    for (int attempt = 0; attempt < 100; ++attempt) {
      makeCopyName(name_buf_[i], attempt, dst, sizeof(dst));
      if (nameTaken(dst)) {
        continue;
      }
      return client_->requestDup(mode_, name_buf_[i], dst);
    }
    return false;
  }

  bool requestRename(size_t i, const char *newName) {
    if (i >= count_ || client_ == nullptr) {
      return false;
    }
    return client_->requestRename(mode_, name_buf_[i], newName);
  }

  bool requestRemove(size_t i) {
    if (i >= count_ || client_ == nullptr) {
      return false;
    }
    return client_->requestDelete(mode_, name_buf_[i]);
  }

private:
  bool nameTaken(const char *n) const {
    for (size_t i = 0; i < count_; ++i) {
      if (std::strcmp(name_buf_[i], n) == 0) {
        return true;
      }
    }
    return false;
  }

  // "peak 245° · ~6:10" from the controller-computed summary facts.
  void buildValue(size_t i) {
    if (peak_[i] <= 0.0F && total_s_[i] == 0) {
      std::strncpy(value_buf_[i], "empty", kValueCap - 1);
      value_buf_[i][kValueCap - 1] = '\0';
      return;
    }
    char peak[16];
    char dur[16];
    profile_facts::formatPeak(peak_[i], fahrenheit_, peak, sizeof(peak));
    profile_facts::formatDuration(total_s_[i], dur, sizeof(dur));
    std::snprintf(value_buf_[i], kValueCap, "%s \xC2\xB7 %s", peak, dur); // \xC2\xB7 = · (U+00B7)
  }

  static void makeCopyName(const char *base, int attempt, char *out, size_t cap) {
    char suffix[16];
    if (attempt == 0) {
      std::snprintf(suffix, sizeof(suffix), " copy");
    } else {
      std::snprintf(suffix, sizeof(suffix), " copy %d", attempt + 1);
    }
    const size_t suffixLen = std::strlen(suffix);
    size_t baseMax = cap > suffixLen + 1 ? cap - suffixLen - 1 : 0;
    size_t baseLen = std::strlen(base);
    if (baseLen > baseMax) {
      baseLen = baseMax;
    }
    size_t n = 0;
    for (; n < baseLen && n + 1 < cap; ++n) {
      out[n] = base[n];
    }
    for (size_t j = 0; j < suffixLen && n + 1 < cap; ++j, ++n) {
      out[n] = suffix[j];
    }
    out[n] = '\0';
  }

  ManagementClient *client_ = nullptr;
  oven_Mode mode_ = oven_Mode_MODE_REFLOW;
  const OvenModel *model_ = nullptr;
  bool fahrenheit_ = false;
  bool mru_sort_ = false; // which order the next requestList() asks the controller for (§23)

  oven_ProfileSort sortWire() const {
    return mru_sort_ ? oven_ProfileSort_PROFILE_SORT_MRU : oven_ProfileSort_PROFILE_SORT_ALPHA;
  }

  // List cache — ONE WINDOW (from ProfileList), rendered in the order the controller returned.
  size_t count_ = 0;
  size_t offset_ = 0; // global index of row 0; controller-reported, never computed here
  size_t total_ = 0;  // profiles in the whole library
  char name_buf_[kMaxRows][kProfileNameCap] = {};
  char value_buf_[kMaxRows][kValueCap] = {};
  bool stock_[kMaxRows] = {};
  float peak_[kMaxRows] = {};
  uint32_t total_s_[kMaxRows] = {};

  // Detail cache (from ProfileData → decoded phases).
  Phase detail_phases_[kMaxPhases] = {};
  size_t detail_count_ = 0;
  bool have_detail_ = false;
};
