// ProfileStore — the CYD's per-mode profile library: the typed Phase[] model + persistence
// (design.md §7, §23; backlog B4). One instance per mode (cure, reflow), each over its own
// IProfileStorage adapter bound to that mode's LittleFS directory (§7 "never mixed"). Serializes a
// profile — name + mode + Phase[] — to a versioned blob through the port, list/load/save/delete/
// duplicate, and enforces the §23 stock-vs-user rule (a stock profile is read-only: save-over and
// delete are refused). Mirrors SettingsStore (B5) structurally — the multi-entry sibling of that
// single-blob store.
//
// The store owns the byte layout and every validity check, so an untrusted blob (a profile pushed
// over serial/WiFi, §7) can only ever be *rejected*, never mis-parsed: load() verifies magic,
// version, mode-match (a cure blob in the reflow dir is ignored), and a phaseCount within bounds,
// and always emits a NUL-terminated name and a phaseCount <= kMaxPhases.
//
// Pure C++: no LVGL, no Arduino, no protobuf — host-tested under native_logic_cyd against a
// FakeProfileStorage. The real LittleFS adapter is thin firmware glue in src_cyd/. A per-mode
// ProfileLibraryViewModel (C4) binds this to lv_subject_t list/selection state; peak/duration row
// facts (§23) are C4's to compute from the loaded phases via the shared curve math (no OvenModel
// dependency here, matching recipe_compiler.h's "policy passed in, never reached into").
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "IProfileStorage.h"
#include "phase.h" // Phase, RecipeMode, kMaxPhases

class ProfileStore {
public:
  ProfileStore(IProfileStorage &storage, RecipeMode mode) : storage_(storage), mode_(mode) {}

  // The typed record the editor (C5) builds and the library (C4) loads.
  struct StoredProfile {
    char name[kProfileNameCap] = {};
    RecipeMode mode = RecipeMode::Reflow;
    bool stock = false;
    size_t phaseCount = 0;
    Phase phases[kMaxPhases] = {};
  };

  // A lightweight list-row descriptor: enough for the §23 list without loading every Phase[]. C4
  // computes peak/duration from a full load() of the highlighted profile.
  struct Summary {
    char name[kProfileNameCap] = {};
    bool stock = false;
    uint16_t phaseCount = 0;
  };

  // Upper bound on how many profiles the store enumerates in one list() call. A §23 mode library on
  // the small screen holds a handful; a larger set is truncated (the caller sees kMaxListed at
  // most).
  static constexpr size_t kMaxListed = 32;

  RecipeMode mode() const { return mode_; }

  // Enumerate this mode's profiles into `out` (at most `cap`), alphabetical by name, skipping any
  // blob that fails validation or belongs to the other mode. Returns the number written.
  size_t list(Summary *out, size_t cap) const {
    ProfileEntry names[kMaxListed];
    size_t total = storage_.list(names, kMaxListed);
    if (total > kMaxListed) {
      total = kMaxListed;
    }

    Summary tmp[kMaxListed];
    size_t m = 0;
    for (size_t i = 0; i < total; ++i) {
      PersistedBlob b;
      if (!loadBlob(names[i].name, b)) {
        continue; // corrupt / wrong-version / other-mode -> not part of this library
      }
      std::strncpy(tmp[m].name, names[i].name, kProfileNameCap - 1);
      tmp[m].name[kProfileNameCap - 1] = '\0';
      tmp[m].stock = b.stock != 0;
      tmp[m].phaseCount = b.phaseCount;
      ++m;
    }

    sortByName(tmp, m);
    const size_t n = m < cap ? m : cap;
    for (size_t i = 0; i < n; ++i) {
      out[i] = tmp[i];
    }
    return n;
  }

  // Load a named profile. Returns false when absent or the stored blob fails validation.
  bool load(const char *name, StoredProfile &out) const {
    PersistedBlob b;
    if (!loadBlob(name, b)) {
      return false;
    }
    copyOut(b, out);
    return true;
  }

  // Persist a profile. Stamps *this store's* mode (never trusts the caller's field across dirs).
  // Refuses an invalid/empty name, an empty or over-long phase list, and — the §23 rule —
  // overwriting an existing stock profile. Returns false on any of those or on a write failure.
  bool save(const StoredProfile &p) {
    if (!validName(p.name)) {
      return false;
    }
    if (p.phaseCount == 0 || p.phaseCount > kMaxPhases) {
      return false;
    }
    PersistedBlob existing;
    if (loadBlob(p.name, existing) && existing.stock) {
      return false; // stock profiles are read-only (Edit -> Save-as at the UI, §23)
    }

    PersistedBlob b{};
    b.magic = kMagic;
    b.version = kVersion;
    b.mode = static_cast<uint8_t>(mode_);
    b.stock = p.stock ? 1 : 0;
    std::strncpy(b.name, p.name, kProfileNameCap - 1);
    b.name[kProfileNameCap - 1] = '\0';
    b.phaseCount = static_cast<uint16_t>(p.phaseCount);
    for (size_t i = 0; i < p.phaseCount; ++i) {
      b.phases[i] = p.phases[i];
    }

    uint8_t buf[sizeof(PersistedBlob)];
    std::memcpy(buf, &b, sizeof(b));
    return storage_.write(p.name, buf, sizeof(buf));
  }

  // Delete a named profile. Refuses stock profiles (§23); a corrupt/foreign blob may still be
  // removed (cleanup). Returns false when refused, absent, or on failure.
  bool remove(const char *name) {
    PersistedBlob b;
    if (loadBlob(name, b) && b.stock) {
      return false; // 🔒 stock (§23)
    }
    return storage_.remove(name);
  }

  // Copy `src` to a new user profile `dst` ("Dup", §23). The copy is always user-owned (stock
  // cleared). Refuses when `src` is missing, `dst` is invalid, or `dst` already exists (no silent
  // clobber). The UI owns the "` copy`" naming + collision resolution; this takes an explicit name.
  bool duplicate(const char *src, const char *dst) {
    StoredProfile p;
    if (!load(src, p)) {
      return false;
    }
    if (!validName(dst)) {
      return false;
    }
    PersistedBlob existing;
    if (loadBlob(dst, existing)) {
      return false; // target name taken
    }
    std::strncpy(p.name, dst, kProfileNameCap - 1);
    p.name[kProfileNameCap - 1] = '\0';
    p.stock = false;
    return save(p);
  }

  // A profile name is also a filename key (§7): non-empty, fits kProfileNameCap, no path separators
  // or control bytes, and not a directory alias. Applied before any name reaches an adapter, so an
  // untrusted (PC/WiFi-uploaded) name can neither traverse the filesystem nor overrun a buffer.
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

  // A phase name (phase.h) is authoring text, NOT a filesystem key — so this is lighter than
  // validName(): non-empty, no control bytes, and fits kPhaseNameCap. No path-separator rule (a
  // phase name never reaches an adapter). Applied at the editor's rename commit; the load path
  // separately NUL-terminates an untrusted blob's phase names (copyOut).
  static bool validPhaseName(const char *name) {
    if (name == nullptr || name[0] == '\0') {
      return false;
    }
    size_t len = 0;
    for (const char *c = name; *c != '\0'; ++c) {
      if (static_cast<unsigned char>(*c) < 0x20) {
        return false; // control / non-printable
      }
      if (++len >= kPhaseNameCap) {
        return false; // no room for the NUL
      }
    }
    return true;
  }

private:
  // Fixed-layout on-flash representation. Explicit magic/version/padding make a layout change a
  // deliberate version bump (an old blob then fails the check and is skipped) — the SettingsStore
  // PersistedBlob contract, applied per profile. `phases` starts at a 4-byte-aligned offset (44) so
  // the trivially-copyable Phase POD memcpys cleanly.
  struct PersistedBlob {
    uint32_t magic;             // "PRO1"
    uint16_t version;           // kVersion
    uint8_t mode;               // RecipeMode
    uint8_t stock;              // 0/1
    char name[kProfileNameCap]; // NUL-terminated; the key, redundantly stored for self-description
    uint16_t phaseCount;
    uint8_t pad_[2];
    Phase phases[kMaxPhases];
  };

  static constexpr uint32_t kMagic = 0x50524F31; // "PRO1"
  static constexpr uint16_t kVersion = 1;

  // Read + validate a named blob. Rejects a short/absent blob, a bad magic or version, a blob whose
  // mode is not this store's (the §7 never-mixed guard, enforced at the store not just the dir),
  // and a phaseCount past the array bound. On success `b` is safe to copyOut().
  bool loadBlob(const char *name, PersistedBlob &b) const {
    uint8_t buf[sizeof(PersistedBlob)];
    const size_t n = storage_.read(name, buf, sizeof(buf));
    if (n != sizeof(PersistedBlob)) {
      return false;
    }
    std::memcpy(&b, buf, sizeof(b));
    if (b.magic != kMagic || b.version != kVersion) {
      return false;
    }
    if (b.mode != static_cast<uint8_t>(mode_)) {
      return false; // wrong mode (or an out-of-range mode byte) -> not part of this library
    }
    if (b.phaseCount > kMaxPhases) {
      return false;
    }
    return true;
  }

  void copyOut(const PersistedBlob &b, StoredProfile &out) const {
    std::memcpy(out.name, b.name, kProfileNameCap);
    out.name[kProfileNameCap - 1] = '\0'; // an untrusted blob's name may lack a terminator
    out.mode = static_cast<RecipeMode>(b.mode);
    out.stock = b.stock != 0;
    // Defense-in-depth: loadBlob already bounded phaseCount, re-clamp so copyOut is safe on its
    // own.
    out.phaseCount = b.phaseCount <= kMaxPhases ? b.phaseCount : kMaxPhases;
    for (size_t i = 0; i < out.phaseCount; ++i) {
      out.phases[i] = b.phases[i];
      out.phases[i].name[kPhaseNameCap - 1] = '\0'; // an untrusted blob's phase name may lack a NUL
    }
  }

  // In-place insertion sort by name (§23 default is recently-used-then-alphabetical; recently-used
  // needs a usage clock the store does not own, so C4's ViewModel re-sorts — the store gives a
  // deterministic alphabetical base). Small n (<= kMaxListed), no heap.
  static void sortByName(Summary *a, size_t n) {
    for (size_t i = 1; i < n; ++i) {
      Summary key = a[i];
      size_t j = i;
      while (j > 0 && std::strcmp(a[j - 1].name, key.name) > 0) {
        a[j] = a[j - 1];
        --j;
      }
      a[j] = key;
    }
  }

  IProfileStorage &storage_;
  RecipeMode mode_;
};
