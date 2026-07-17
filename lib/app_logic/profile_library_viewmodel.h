// profile_library_viewmodel.h — the per-mode logic behind the §23 profile library (backlog C4).
//
// One instance per mode (cure, reflow) over that mode's ProfileStore. It turns the store's contents
// into what the library screen renders: a list of rows (name + "peak 245° · ~6:10" facts computed
// from the §12 curve math), the detail facts + curve points for a highlighted profile, and the
// store-mutating actions the screen's buttons drive (Dup with " copy" naming/collision resolution,
// Delete). The New/Edit/Load actions leave for screens C4 does not own (editor §12/C5, Setup
// §19/C6); those are the screen's to publish as a NavRequest, not this model's.
//
// Pure C++ — no LVGL, no Arduino. Selection state lives in the screen's SelectableListModel (which
// owns the lv_subject_t), so this model stays a plain fact/action provider, host-tested under
// native_logic_cyd against a FakeProfileStorage. The screen (ui_logic) copies rowLabel/rowValue
// into SelectableListItems; the string buffers are members here so those borrowed pointers stay
// valid while the list is shown (the SettingsScreen *_value_[] pattern).
#pragma once

#include <cstddef>
#include <cstdio>
#include <cstring>

#include "oven_cal.h"
#include "profile_facts.h"
#include "profile_store.h"

class ProfileLibraryViewModel {
public:
  static constexpr size_t kMaxRows = ProfileStore::kMaxListed;
  static constexpr size_t kValueCap = 24; // "peak 245° · ~6:10" + NUL, with °/· as 2 bytes each

  // Default-constructed then init()'d, so a screen can hold one per mode as a member and bind the
  // stores in begin() (the value_stepper_viewmodel idiom). The convenience ctor is for tests.
  ProfileLibraryViewModel() = default;
  ProfileLibraryViewModel(ProfileStore &store, const OvenModel &model) { init(store, model); }

  // `model` supplies the calibration the facts/curve are computed against (oven_cal::kDefaultModel
  // in production; a toy model in tests). Both must outlive this object.
  void init(ProfileStore &store, const OvenModel &model) {
    store_ = &store;
    model_ = &model;
    fahrenheit_ = false;
    count_ = 0;
  }

  RecipeMode mode() const { return store_->mode(); }

  // Temperature unit for the row/detail facts — set by the screen from subj_units (this pure model
  // cannot read the subject). Applied on the next refresh().
  void setFahrenheit(bool f) { fahrenheit_ = f; }
  bool fahrenheit() const { return fahrenheit_; }

  // Reload the list from the store (alphabetical — recently-used is deferred, no usage clock) and
  // recompute each row's fact string. Call on entry and after any mutating action.
  void refresh() {
    ProfileStore::Summary rows[kMaxRows];
    count_ = store_->list(rows, kMaxRows);
    for (size_t i = 0; i < count_; ++i) {
      std::strncpy(name_buf_[i], rows[i].name, kProfileNameCap - 1);
      name_buf_[i][kProfileNameCap - 1] = '\0';
      stock_[i] = rows[i].stock;
      buildValue(i);
    }
  }

  size_t count() const { return count_; }
  const char *rowLabel(size_t i) const { return i < count_ ? name_buf_[i] : ""; }
  const char *rowValue(size_t i) const { return i < count_ ? value_buf_[i] : ""; }
  const char *name(size_t i) const { return rowLabel(i); }
  bool rowStock(size_t i) const { return i < count_ && stock_[i]; }

  // A stock profile is read-only (§23): Delete is disabled and Edit becomes Save-as.
  bool canDelete(size_t i) const { return i < count_ && !stock_[i]; }
  bool editIsSaveAs(size_t i) const { return rowStock(i); }

  // Load the highlighted profile's full record (for the detail screen). False if the index is out
  // of range or the blob fails validation.
  bool loadDetail(size_t i, ProfileStore::StoredProfile &out) const {
    if (i >= count_) {
      return false;
    }
    return store_->load(name_buf_[i], out);
  }

  profile_facts::ProfileFacts facts(size_t i) const {
    ProfileStore::StoredProfile p;
    if (!loadDetail(i, p)) {
      return {};
    }
    return profile_facts::computeFacts(p.phases, p.phaseCount, store_->mode(), *model_);
  }

  // The count of *authored* phases (clamped to kMaxPhases) — what the curve's phase-name labels
  // index (via phaseNames()). The curve's boundaries carry one extra entry (the implicit cool-down,
  // §6) when the run ends hot; the caller appends "Cool" for that, so it must not fold it into the
  // count.
  size_t phaseCount(size_t i) const {
    ProfileStore::StoredProfile p;
    if (!loadDetail(i, p)) {
      return 0;
    }
    return p.phaseCount > kMaxPhases ? kMaxPhases : p.phaseCount;
  }

  // Copy each authored phase's stored name (phase.h) for profile `i` into `out` (writing at most
  // `cap`, each NUL-terminated), for the detail curve's phase-name labels. One store load; returns
  // the number written.
  size_t phaseNames(size_t i, char (*out)[kPhaseNameCap], size_t cap) const {
    ProfileStore::StoredProfile p;
    if (!loadDetail(i, p)) {
      return 0;
    }
    size_t n = p.phaseCount > kMaxPhases ? kMaxPhases : p.phaseCount;
    if (n > cap) {
      n = cap;
    }
    for (size_t k = 0; k < n; ++k) {
      std::strncpy(out[k], p.phases[k].name, kPhaseNameCap - 1);
      out[k][kPhaseNameCap - 1] = '\0';
    }
    return n;
  }

  // Fill the detail curve's requested / achievable point series for profile `i`. Returns point
  // count.
  size_t sampleRequested(size_t i, profile_facts::CurvePoint *out, size_t cap) const {
    return sample(i, /*achievable=*/false, out, cap);
  }
  // Phase-boundary times (achievable timeline) for the detail curve's phase separators (§12).
  size_t samplePhaseBoundaries(size_t i, float *out, size_t cap) const {
    ProfileStore::StoredProfile p;
    if (!loadDetail(i, p)) {
      return 0;
    }
    return profile_facts::samplePhaseBoundaries(p.phases, p.phaseCount, store_->mode(), *model_,
                                                profile_facts::kDefaultAmbientC, out, cap);
  }
  // UV-on time windows for the detail curve's shading (cure profiles, §12).
  size_t sampleUvSpans(size_t i, profile_facts::TimeSpan *out, size_t cap) const {
    ProfileStore::StoredProfile p;
    if (!loadDetail(i, p)) {
      return 0;
    }
    return profile_facts::sampleUvSpans(p.phases, p.phaseCount, store_->mode(), *model_,
                                        profile_facts::kDefaultAmbientC, out, cap);
  }
  // Closed-loop settling (predicted actual temperature) for the detail curve — the same trace the
  // editor draws, so both charts match (§12).
  size_t sampleOvershoot(size_t i, profile_facts::CurvePoint *out, size_t cap) const {
    ProfileStore::StoredProfile p;
    if (!loadDetail(i, p)) {
      return 0;
    }
    return profile_facts::sampleOvershoot(p.phases, p.phaseCount, store_->mode(), *model_,
                                          profile_facts::kDefaultAmbientC, out, cap);
  }
  bool uncalibrated() const { return !model_->calibrated; }

  // Duplicate profile `i` as "<name> copy" (or " copy 2", " copy 3", … on a collision), always a
  // user-owned (editable/deletable) copy. Refreshes the list on success. Returns false if `i` is
  // out of range or a unique name could not be formed.
  bool duplicate(size_t i) {
    if (i >= count_) {
      return false;
    }
    char dst[kProfileNameCap];
    for (int attempt = 0; attempt < 100; ++attempt) {
      makeCopyName(name_buf_[i], attempt, dst, sizeof(dst));
      if (!ProfileStore::validName(dst)) {
        continue;
      }
      if (store_->duplicate(name_buf_[i], dst)) { // refuses an existing dst → try the next number
        refresh();
        return true;
      }
    }
    return false;
  }

  // Delete profile `i` (user profiles only — the store refuses stock). Refreshes on success.
  bool remove(size_t i) {
    if (i >= count_ || !store_->remove(name_buf_[i])) {
      return false;
    }
    refresh();
    return true;
  }

private:
  size_t sample(size_t i, bool achievable, profile_facts::CurvePoint *out, size_t cap) const {
    ProfileStore::StoredProfile p;
    if (!loadDetail(i, p)) {
      return 0;
    }
    return profile_facts::sampleCurve(p.phases, p.phaseCount, store_->mode(), *model_, achievable,
                                      profile_facts::kDefaultAmbientC, out, cap);
  }

  // "peak 245° · ~6:10" — the §23 row facts, or "empty" for a profile with no usable phases.
  void buildValue(size_t i) {
    const profile_facts::ProfileFacts f = facts(i);
    if (f.phaseCount == 0) {
      std::strncpy(value_buf_[i], "empty", kValueCap - 1);
      value_buf_[i][kValueCap - 1] = '\0';
      return;
    }
    char peak[16];
    char dur[16];
    profile_facts::formatPeak(f.peakC, fahrenheit_, peak, sizeof(peak));
    profile_facts::formatDuration(f.totalSeconds, dur, sizeof(dur));
    std::snprintf(value_buf_[i], kValueCap, "%s \xC2\xB7 %s", peak, dur); // \xC2\xB7 = · (U+00B7)
  }

  // Form the `attempt`-th copy name for `base`: attempt 0 → "<base> copy", attempt k → "<base> copy
  // (k+1)". The base is truncated so the whole thing fits kProfileNameCap (incl. the NUL).
  static void makeCopyName(const char *base, int attempt, char *out, size_t cap) {
    char suffix[16];
    if (attempt == 0) {
      std::snprintf(suffix, sizeof(suffix), " copy");
    } else {
      std::snprintf(suffix, sizeof(suffix), " copy %d", attempt + 1);
    }
    const size_t suffixLen = std::strlen(suffix);
    size_t baseMax = cap > suffixLen + 1 ? cap - suffixLen - 1 : 0; // room for base + suffix + NUL
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

  ProfileStore *store_ = nullptr;
  const OvenModel *model_ = nullptr;
  bool fahrenheit_ = false;
  size_t count_ = 0;
  char name_buf_[kMaxRows][kProfileNameCap] = {};
  char value_buf_[kMaxRows][kValueCap] = {};
  bool stock_[kMaxRows] = {};
};
