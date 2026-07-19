// Controller-side profile library store (design.md §7, §23; Wave R2 of the §2 "CYD is a UI
// remote" split, 2026-07-17). Owns the on-flash byte layout for one mode's library over an
// IProfileStorage adapter, storing each profile as a small versioned header + a nanopb-encoded
// oven_Profile keyed by name. The multi-entry sibling of the settings blob; the controller
// analogue of the CYD's old lib/app_logic/profile_store.h, which the CYD sheds in R3.
//
// This now lives on the SAFETY MCU, and a profile arrives from two untrusted sources — the flash
// blob AND a wire ProfilePut (§9) — so a malformed input is *rejected, never mis-parsed*: the
// header is checked, the body is decoded through nanopb (whose static allocation bounds every
// repeated/string field by construction, oven.options), the decoded mode must be this store's
// (the §7 never-mixed guard, enforced at the store, not just the directory), and a name is
// validated (no path traversal/overrun) before it ever reaches an adapter. nanopb NUL-terminates
// every decoded string field, so no phase/profile name can run off its buffer.
//
// Pure C++ over nanopb — no Arduino, no LVGL — host-tested under native_control against a
// FakeProfileStorage. The real LittleFS adapter is thin firmware glue in src_control/.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "IProfileStorage.h"
#include "codec.h"
#include "oven.pb.h"

namespace control {

// Derived list-row facts (design.md §23) the controller computes so one ProfileListReq renders the
// whole library. peak_c is exact (hottest phase target); total_s is an approximate sum of the
// authored ramp+hold seconds — the CYD's detail view recomputes the precise ∫dT/rate projection
// from the fetched Profile (profile_facts.h), so this is only the row's "~mm:ss".
struct ProfileFacts {
  float peak_c = 0.0F;
  uint32_t total_s = 0;
};

class ProfileStore {
public:
  // One store per mode (cure/reflow), over that mode's IProfileStorage adapter (its own LittleFS
  // directory, §7). storage must outlive this store.
  ProfileStore(IProfileStorage &storage, oven_Mode mode) : storage_(storage), mode_(mode) {}

  oven_Mode mode() const { return mode_; }

  // Row ordering for list() (design.md §23). Alpha is the base; Mru is newest-first by the recency
  // counter (Setup → Load picker). The controller sorts because it owns use_seq and the DRAM.
  enum class SortMode { Alpha, Mru };

  // Upper bound on how many profiles list() enumerates (design.md §23; == ProfileStore::kMaxListed
  // on the CYD and oven.ProfileList.profiles max_count). A larger set is truncated.
  static constexpr size_t kMaxListed = 32;

  // The nanopb static bound on a Profile's phases (oven.options oven.Profile.phases max_count).
  // Local so this header needn't pull the CYD's phase.h; asserted against the generated array.
  static constexpr size_t kMaxPhases = 32;
  static_assert(sizeof(oven_Profile::phases) == kMaxPhases * sizeof(oven_Phase),
                "kMaxPhases must match oven.options oven.Profile.phases max_count");

  // The list row: name (key), stock flag, the derived facts, and the recency rank (§23 MRU sort).
  struct Summary {
    char name[kProfileNameCap] = {};
    bool stock = false;
    ProfileFacts facts;
    uint32_t use_seq = 0; // controller-stamped recency counter; the CYD's MRU sort key
  };

  // Load + validate a named profile into `out`. False when absent, malformed, or belonging to
  // another mode. On success `out` is a fully-decoded, bounded oven_Profile.
  bool load(const char *name, oven_Profile &out) const { return loadBlob(name, out); }

  // Does a valid, same-mode profile exist under `name`? (For the responder's NAK-reason mapping.)
  bool contains(const char *name) const {
    oven_Profile b = oven_Profile_init_zero;
    return loadBlob(name, b);
  }

  // Is the named profile a stock (read-only, §23) one? False if absent/invalid.
  bool isStock(const char *name) const {
    oven_Profile b = oven_Profile_init_zero;
    return loadBlob(name, b) && b.stock;
  }

  // Persist a profile. Stamps *this store's* mode (never trusts the caller's field across dirs).
  // Refuses an invalid/empty name, an empty or over-long phase list, and — the §23 rule —
  // overwriting an existing stock profile. Returns false on any of those or a write failure.
  bool save(const oven_Profile &p) {
    if (!validName(p.name)) {
      return false;
    }
    if (p.phases_count == 0 || p.phases_count > kMaxPhases) {
      return false;
    }
    oven_Profile existing = oven_Profile_init_zero;
    if (loadBlob(p.name, existing) && existing.stock) {
      return false; // stock profiles are read-only (Edit -> Save-as at the UI, §23)
    }
    oven_Profile stamped = p;
    stamped.mode = mode_; // authoritative: the store's dir decides the mode, not the caller
    stamped.use_seq = nextUseSeq(); // controller owns recency; any wire-supplied use_seq is ignored
    return writeBlob(stamped);
  }

  // Mark a profile "used" (run-start, §23): bump its recency counter so the CYD's MRU sort floats
  // it to the top. Unlike save(), this is allowed on STOCK profiles — running a stock profile is a
  // use — so it goes straight to writeBlob (which does not enforce the read-only rule). Returns
  // false only if the profile is absent/malformed.
  bool touch(const char *name) {
    oven_Profile p = oven_Profile_init_zero;
    if (!loadBlob(name, p)) {
      return false;
    }
    p.use_seq = nextUseSeq();
    return writeBlob(p);
  }

  // Delete a named profile. Refuses stock (§23); a corrupt/foreign blob may still be removed
  // (cleanup). Returns false when refused, absent, or on failure.
  bool remove(const char *name) {
    oven_Profile b = oven_Profile_init_zero;
    if (loadBlob(name, b) && b.stock) {
      return false; // stock (§23)
    }
    return storage_.remove(name);
  }

  // Copy `src` to a new user profile `dst` ("Dup", §23) — always user-owned (stock cleared).
  // Refuses a missing `src`, an invalid `dst`, or a `dst` that already exists (no silent clobber).
  // The CYD pre-deconflicts the name against its cached list (§9), so a collision here is a race.
  bool duplicate(const char *src, const char *dst) {
    oven_Profile p = oven_Profile_init_zero;
    if (!load(src, p)) {
      return false;
    }
    if (!validName(dst) || contains(dst)) {
      return false;
    }
    setName(p, dst);
    p.stock = false;
    return save(p);
  }

  // Rename user profile `src` to `dst`, preserving content. Refuses stock (§23), a missing `src`,
  // an invalid `dst`, or a `dst` that already exists. Writes the new file *then* removes the old,
  // so an interrupted rename keeps the profile under its old name rather than losing it.
  bool rename(const char *src, const char *dst) {
    if (!validName(dst)) {
      return false;
    }
    oven_Profile p = oven_Profile_init_zero;
    if (!load(src, p) || p.stock) {
      return false;
    }
    if (std::strcmp(src, dst) == 0) {
      return true; // no-op
    }
    if (contains(dst)) {
      return false;
    }
    setName(p, dst);
    if (!save(p)) {
      return false;
    }
    return storage_.remove(src);
  }

  // Enumerate this mode's profiles into `out` (at most `cap`), ordered per `sort` (alphabetical, or
  // most-recently-used newest-first), skipping any blob that fails validation or belongs to the
  // other mode. Returns the number written.
  size_t list(Summary *out, size_t cap, SortMode sort = SortMode::Alpha) const {
    ProfileEntry names[kMaxListed];
    size_t total = storage_.list(names, kMaxListed);
    if (total > kMaxListed) {
      total = kMaxListed;
    }
    Summary tmp[kMaxListed];
    size_t m = 0;
    for (size_t i = 0; i < total; ++i) {
      oven_Profile p = oven_Profile_init_zero;
      if (!loadBlob(names[i].name, p)) {
        continue; // corrupt / wrong-version / other-mode -> not part of this library
      }
      std::strncpy(tmp[m].name, names[i].name, kProfileNameCap - 1);
      tmp[m].name[kProfileNameCap - 1] = '\0';
      tmp[m].stock = p.stock;
      tmp[m].facts = facts(p);
      tmp[m].use_seq = p.use_seq;
      ++m;
    }
    sortRows(tmp, m, sort);
    const size_t n = m < cap ? m : cap;
    for (size_t i = 0; i < n; ++i) {
      out[i] = tmp[i];
    }
    return n;
  }

  // Derived facts for a decoded profile (peak target; approximate total seconds). Static so the
  // seed generator (tools/gen_profiles.cpp) and tests can reuse it.
  static ProfileFacts facts(const oven_Profile &p) {
    ProfileFacts f;
    double total = 0.0;
    const size_t n = p.phases_count <= kMaxPhases ? p.phases_count : kMaxPhases;
    for (size_t i = 0; i < n; ++i) {
      if (p.phases[i].target_c > f.peak_c) {
        f.peak_c = p.phases[i].target_c;
      }
      const float ramp = p.phases[i].ramp_s > 0.0F ? p.phases[i].ramp_s : 0.0F;
      const float hold = p.phases[i].hold_s > 0.0F ? p.phases[i].hold_s : 0.0F;
      total += static_cast<double>(ramp) + static_cast<double>(hold);
    }
    f.total_s = total > 0.0 ? static_cast<uint32_t>(total) : 0U;
    return f;
  }

  // A profile name is also a filename key (§7): non-empty, fits kProfileNameCap, no path separators
  // or control bytes, and not a directory alias. Applied before any name reaches an adapter, so an
  // untrusted (wire/PC-uploaded) name can neither traverse the filesystem nor overrun a buffer.
  static bool validName(const char *name) {
    if (name == nullptr || name[0] == '\0') {
      return false;
    }
    if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0) {
      return false;
    }
    size_t len = 0;
    for (const char *c = name; *c != '\0'; ++c) {
      if (*c == '/' || *c == '\\') {
        return false;
      }
      if (static_cast<unsigned char>(*c) < 0x20) {
        return false; // control / non-printable
      }
      if (++len >= kProfileNameCap) {
        return false; // no room for the NUL
      }
    }
    return true;
  }

  // Encode a profile to its on-flash blob (header + nanopb body) into `buf`. Static so the seed
  // generator produces byte-identical blobs. Returns false only if the buffer is too small.
  static bool encodeBlob(const oven_Profile &p, uint8_t *buf, size_t cap, size_t &out_len) {
    if (cap < kHeaderLen) {
      return false;
    }
    writeHeader(buf);
    size_t body = 0;
    if (!protocol::encode(oven_Profile_fields, &p, buf + kHeaderLen, cap - kHeaderLen, body)) {
      return false;
    }
    out_len = kHeaderLen + body;
    return true;
  }

  // Blob capacity a caller must provide (header + the largest nanopb Profile).
  static constexpr size_t kBlobCap = 6 /*header*/ + oven_Profile_size;

private:
  static constexpr uint32_t kMagic = 0x50524F32; // "PRO2" — nanopb format (distinct from the
                                                 // CYD's old memcpy "PRO1")
  static constexpr uint16_t kVersion = 1;
  static constexpr size_t kHeaderLen = 6; // magic(4) + version(2), little-endian

  static void writeHeader(uint8_t *buf) {
    buf[0] = static_cast<uint8_t>(kMagic & 0xFF);
    buf[1] = static_cast<uint8_t>((kMagic >> 8) & 0xFF);
    buf[2] = static_cast<uint8_t>((kMagic >> 16) & 0xFF);
    buf[3] = static_cast<uint8_t>((kMagic >> 24) & 0xFF);
    buf[4] = static_cast<uint8_t>(kVersion & 0xFF);
    buf[5] = static_cast<uint8_t>((kVersion >> 8) & 0xFF);
  }

  static bool checkHeader(const uint8_t *buf, size_t len) {
    if (len < kHeaderLen) {
      return false;
    }
    const uint32_t magic = static_cast<uint32_t>(buf[0]) | (static_cast<uint32_t>(buf[1]) << 8) |
                           (static_cast<uint32_t>(buf[2]) << 16) |
                           (static_cast<uint32_t>(buf[3]) << 24);
    const uint16_t version = static_cast<uint16_t>(buf[4]) | (static_cast<uint16_t>(buf[5]) << 8);
    return magic == kMagic && version == kVersion;
  }

  // Read + validate + decode a named blob into `out`. Rejects a short/absent blob, a bad header,
  // a body nanopb can't decode, and a profile whose mode is not this store's.
  bool loadBlob(const char *name, oven_Profile &out) const {
    uint8_t buf[kBlobCap];
    const size_t n = storage_.read(name, buf, sizeof(buf));
    if (!checkHeader(buf, n)) {
      return false;
    }
    out = oven_Profile_init_zero;
    if (!protocol::decode(oven_Profile_fields, &out, buf + kHeaderLen, n - kHeaderLen)) {
      return false;
    }
    // The §7 never-mixed guard at the store: an out-of-range mode byte or the other mode's profile
    // (a cross-mode upload landing in the wrong dir) is not part of this library.
    if (protocol::wireEnum(out.mode) != static_cast<int32_t>(mode_)) {
      return false;
    }
    return true;
  }

  // The next recency stamp: one past the highest use_seq currently stored in this mode's library
  // (1 for an empty library). Because recency is relative, deriving it from the stored max — rather
  // than a separately persisted counter — keeps the whole feature self-contained in the store: the
  // order among surviving profiles is preserved, and there is no extra persistent state to corrupt.
  // O(n) over n <= kMaxListed blobs, only on a save/touch (rare, user-driven). Wraps at 2^32, which
  // is astronomically far off; the comparator tolerates equal/rolled values.
  uint32_t nextUseSeq() const {
    ProfileEntry names[kMaxListed];
    size_t total = storage_.list(names, kMaxListed);
    if (total > kMaxListed) {
      total = kMaxListed;
    }
    uint32_t mx = 0;
    for (size_t i = 0; i < total; ++i) {
      oven_Profile p = oven_Profile_init_zero;
      if (loadBlob(names[i].name, p) && p.use_seq > mx) {
        mx = p.use_seq;
      }
    }
    return mx + 1U;
  }

  bool writeBlob(const oven_Profile &p) {
    uint8_t buf[kBlobCap];
    size_t len = 0;
    if (!encodeBlob(p, buf, sizeof(buf), len)) {
      return false;
    }
    return storage_.write(p.name, buf, len);
  }

  static void setName(oven_Profile &p, const char *name) {
    std::strncpy(p.name, name, sizeof(p.name) - 1);
    p.name[sizeof(p.name) - 1] = '\0';
  }

  // In-place insertion sort of the rows for `sort` (§23). Alpha orders by name; Mru orders by the
  // recency counter newest-first, tie-broken by name so equal/unset (0) seqs stay deterministic (a
  // fresh all-stock library falls back to alphabetical). use_seq is a plain uint32: higher ==
  // newer; a value that wrapped past 2^32 reads as oldest. Small n (<= kMaxListed), no heap.
  static void sortRows(Summary *a, size_t n, SortMode sort) {
    for (size_t i = 1; i < n; ++i) {
      Summary key = a[i];
      size_t j = i;
      while (j > 0 && rowAfter(a[j - 1], key, sort)) {
        a[j] = a[j - 1];
        --j;
      }
      a[j] = key;
    }
  }

  // Does row `x` sort AFTER row `y` (so `y` should come first) under `sort`?
  static bool rowAfter(const Summary &x, const Summary &y, SortMode sort) {
    if (sort == SortMode::Mru && x.use_seq != y.use_seq) {
      return x.use_seq < y.use_seq; // newer (higher) first
    }
    return std::strcmp(x.name, y.name) > 0; // name asc (Alpha, and the MRU tie-break)
  }

  IProfileStorage &storage_;
  oven_Mode mode_;
};

} // namespace control
