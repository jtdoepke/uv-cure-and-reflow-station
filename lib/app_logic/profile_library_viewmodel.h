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
#include "profile_facts.h"

class ProfileLibraryViewModel {
public:
  // Max list rows == the wire ProfileList bound (oven.options) == the controller store's
  // kMaxListed.
  static constexpr size_t kMaxRows = 32;
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

  // Ask the controller for this mode's library, in the current sort order (§23). Returns false if
  // the client is busy. The screen shows a loading state, polls the client, and calls adoptList()
  // when it is Ready.
  bool requestList() {
    return client_ != nullptr &&
           client_->requestList(mode_, mru_sort_ ? oven_ProfileSort_PROFILE_SORT_MRU
                                                 : oven_ProfileSort_PROFILE_SORT_ALPHA);
  }

  // Adopt a ProfileList reply into the row cache (call from the screen's poll on Ready).
  void adoptList(const oven_ProfileList &list) {
    count_ = list.profiles_count;
    if (count_ > kMaxRows) {
      count_ = kMaxRows;
    }
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

  size_t count() const { return count_; }
  const char *rowLabel(size_t i) const { return i < count_ ? name_buf_[i] : ""; }
  const char *rowValue(size_t i) const { return i < count_ ? value_buf_[i] : ""; }
  const char *name(size_t i) const { return rowLabel(i); }
  bool rowStock(size_t i) const { return i < count_ && stock_[i]; }
  bool canDelete(size_t i) const { return i < count_ && !stock_[i]; }
  bool editIsSaveAs(size_t i) const { return rowStock(i); }

  // Is `name` already in the current cached list? Lets the rename UI reject an obvious clash
  // client-side (staying on the keyboard) rather than round-tripping for a NAK (§23). The
  // controller still validates authoritatively.
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

  // Duplicate row `i` as "<name> copy" (or " copy 2", …), deconflicting against the CURRENT cached
  // list (the controller also refuses a clash, §23). Returns false if `i` is out of range, no free
  // name was found, or the client is busy.
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

  // List cache (from ProfileList), rendered in the order the controller returned.
  size_t count_ = 0;
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
