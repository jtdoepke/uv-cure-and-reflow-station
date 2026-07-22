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

  // How many profiles one mode's library may hold (design.md §23). A larger set is truncated.
  //
  // REVISED 2026-07-22, 32 -> 64: this used to be pinned to oven.ProfileList.profiles max_count,
  // because one reply carried the whole library — so the TinyFrame payload budget was what capped
  // the library (1542 B at 32 rows against TF_MAX_PAYLOAD_RX=2048 left room for only 42). Now the
  // list is PAGED: the wire carries a 16-row window (kListWindow) and this is purely a storage
  // bound. The two numbers are deliberately no longer equal; do not re-couple them.
  //
  // What this DOES still cost is the sort: paging correctly means ordering the whole library
  // before slicing a window, so the scratch below is sized to this.
  static constexpr size_t kMaxListed = 64;

  // Rows in one ProfileList reply — the paging window (== oven.ProfileList.profiles max_count).
  // Sized so a reply stays ~790 B, comfortably inside TF_MAX_PAYLOAD_RX; the §23 list shows ~4-5
  // rows at a time, so a window is ~3 screens of scrolling per round-trip.
  static constexpr size_t kListWindow = 16;

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
  bool contains(const char *name) const { return loadBlob(name, scratch().probe); }

  // Is the named profile a stock (read-only, §23) one? False if absent/invalid.
  bool isStock(const char *name) const {
    return loadBlob(name, scratch().probe) && scratch().probe.stock;
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
    if (loadBlob(p.name, scratch().probe) && scratch().probe.stock) {
      return false; // stock profiles are read-only (Edit -> Save-as at the UI, §23)
    }
    oven_Profile &stamped = scratch().stamped;
    stamped = p;
    stamped.mode = mode_; // authoritative: the store's dir decides the mode, not the caller
    stamped.use_seq = nextUseSeq(); // controller owns recency; any wire-supplied use_seq is ignored
    return writeBlob(stamped);
  }

  // What a seedStock() attempt did, so the caller can report it honestly rather than collapsing
  // "already there" and "refused" into one failure.
  enum class SeedOutcome : uint8_t {
    Written,   // the profile was absent (or overwrite was asked for) and is now stored
    Present,   // an identical-named stock profile already exists; nothing to do
    UserOwned, // a USER profile holds this name — left alone, never clobbered
    Failed,    // encode or write failure
  };

  // Install a factory profile from the firmware's own compiled-in stock table (§23's "the factory
  // references can't be lost"). Deliberately NOT routed through save(), which refuses to overwrite
  // a stock profile — correct for every other caller, and exactly what a restore has to do.
  //
  // **This is a privileged write, and its safety is entirely in who may call it.** The only
  // legitimate source is stock_profiles.h, compiled into this firmware and reviewed with it. A
  // profile arriving over the wire must never reach here: the CYD may *ask* for a restore (§9
  // ProfileRestoreStock) but can never *supply* what gets written, so a malicious or confused peer
  // cannot use the restore path to plant an unremovable read-only profile.
  //
  // Never clobbers a USER profile that happens to hold the name — the operator's work outranks a
  // factory reference, and §23's promise is about not LOSING the stock set, not about owning the
  // namespace. `overwrite` only governs replacing an existing STOCK entry (the repair case).
  SeedOutcome seedStock(const oven_Profile &p, bool overwrite) {
    if (!validName(p.name) || p.phases_count == 0 || p.phases_count > kMaxPhases) {
      return SeedOutcome::Failed;
    }
    if (loadBlob(p.name, scratch().probe)) {
      if (!scratch().probe.stock) {
        return SeedOutcome::UserOwned;
      }
      if (!overwrite) {
        return SeedOutcome::Present;
      }
    }
    oven_Profile &stamped = scratch().stamped;
    stamped = p;
    stamped.mode = mode_; // authoritative, as in save(): the directory decides the mode
    stamped.stock = true; // a seeded profile is stock by definition, whatever the table said
    stamped.use_seq = nextUseSeq();
    return writeBlob(stamped) ? SeedOutcome::Written : SeedOutcome::Failed;
  }

  // Mark a profile "used" (run-start, §23): bump its recency counter so the CYD's MRU sort floats
  // it to the top. Unlike save(), this is allowed on STOCK profiles — running a stock profile is a
  // use — so it goes straight to writeBlob (which does not enforce the read-only rule). Returns
  // false only if the profile is absent/malformed.
  bool touch(const char *name) {
    oven_Profile &p = scratch().stamped; // the outgoing copy, same role as in save()
    if (!loadBlob(name, p)) {
      return false;
    }
    p.use_seq = nextUseSeq();
    return writeBlob(p);
  }

  // Delete a named profile. Refuses stock (§23); a corrupt/foreign blob may still be removed
  // (cleanup). Returns false when refused, absent, or on failure.
  bool remove(const char *name) {
    if (loadBlob(name, scratch().probe) && scratch().probe.stock) {
      return false; // stock (§23)
    }
    if (!storage_.remove(name)) {
      return false;
    }
    indexRemove(name);
    return true;
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
    if (!storage_.remove(src)) {
      return false;
    }
    indexRemove(src); // save() added the new key; drop the old one
    return true;
  }

  // What a paged list() actually returned, so the caller never has to re-derive it (§23). The CYD
  // renders `written` rows starting at `offset` and knows it has reached the end when
  // offset + written >= total — it does no page arithmetic of its own.
  struct Page {
    size_t written = 0; // rows placed in `out`
    size_t offset = 0;  // the offset ACTUALLY used: clamped, or resolved from an anchor
    size_t total = 0;   // valid profiles in this mode's library (not in this reply)
  };

  // Enumerate one WINDOW of this mode's profiles into `out` (at most `cap`), ordered per `sort`
  // (alphabetical, or most-recently-used newest-first), skipping any blob that fails validation or
  // belongs to the other mode.
  //
  // `offset` is clamped into [0, total-1] and the value used comes back in Page::offset — so a
  // caller paging through a library that shrank under it (a delete that emptied the last page)
  // self-corrects instead of rendering a phantom row. `anchor` (optional) overrides `offset` with
  // the window CONTAINING that name, which is how the CYD keeps a row in view across a mutation
  // without tracking page numbers; an anchor that is absent falls back to `offset`.
  //
  // Paging is only coherent because both orders are TOTAL orders: Alpha is unique-by-name (names
  // are the store's filesystem key) and Mru ties break by name (see rowAfter). If either ever
  // admitted equal rows, windows would overlap and rows would duplicate or vanish between pages.
  Page listPage(Summary *out, size_t cap, SortMode sort = SortMode::Alpha, size_t offset = 0,
                const char *anchor = nullptr) const {
    const size_t m = collectSorted(sort);
    Page pg;
    pg.total = m;
    if (m == 0 || cap == 0) {
      return pg;
    }
    size_t start = offset;
    if (anchor != nullptr && anchor[0] != '\0') {
      for (size_t i = 0; i < m; ++i) {
        if (std::strcmp(scratch().rows[i].name, anchor) == 0) {
          // Snap to the window boundary the anchor falls in, so repeated refreshes on the same
          // anchor are stable rather than drifting the window by one row each time.
          start = (i / cap) * cap;
          break;
        }
      }
    }
    if (start >= m) {
      start = m - 1; // clamp; the caller learns the truth from Page::offset
    }
    size_t n = m - start;
    if (n > cap) {
      n = cap;
    }
    for (size_t i = 0; i < n; ++i) {
      out[i] = scratch().rows[start + i];
    }
    pg.written = n;
    pg.offset = start;
    return pg;
  }

  // The unpaged form: the first `cap` rows in `sort` order, returning how many were written. What
  // every caller that just wants "the library" (boot logging, tests) should use; the responder uses
  // listPage() because only the wire needs windowing.
  size_t list(Summary *out, size_t cap, SortMode sort = SortMode::Alpha) const {
    return listPage(out, cap, sort).written;
  }

  // How many valid profiles this mode's library holds. Same walk as listPage(), without the copy.
  size_t count() const { return collectSorted(SortMode::Alpha); }

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
    // Reserved: the per-mode index file shares this directory (see kIndexName). Refusing the name
    // here is what makes "the index is not a profile" true by construction rather than by a filter.
    if (std::strcmp(name, kIndexName) == 0) {
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

  static constexpr uint32_t kMagic = 0x50524F32; // "PRO2" — nanopb format (distinct from the
                                                 // CYD's old memcpy "PRO1")
  static constexpr uint16_t kVersion = 1;
  static constexpr size_t kHeaderLen = 6; // magic(4) + version(2), little-endian

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

  // Blob capacity a caller must provide (header + the largest nanopb Profile). Sized to the v2
  // header, which is the only one written; a v1 blob is shorter and reads fine into this.
  static constexpr size_t kBlobCap = kHeaderLen + oven_Profile_size;

private:
  static constexpr uint32_t kIndexMagic = 0x31584449; // "IDX1"
  static constexpr uint16_t kIndexVersion = 1;
  static constexpr size_t kIndexHeaderLen = 10; // magic(4) version(2) mode(1) pad(1) count(2)
  static constexpr size_t kIndexRecLen =
      kProfileNameCap + 1 + 4 + 4 + 4; // name stock peak total seq
  static constexpr size_t kIndexCap = kIndexHeaderLen + kMaxListed * kIndexRecLen;

  // The index lives beside the profiles, under a name validName() refuses so no profile can ever
  // collide with it (see validName) and list() can skip it by name.
  static constexpr const char *kIndexName = "__index";

  // Shared working memory, deliberately in .bss rather than on the caller's stack.
  //
  // WHY: these buffers are large (a decoded oven_Profile is ~1.3 KB, the encode/decode blob ~1.5
  // KB) and two of them scale with kMaxListed. On the stack that made list() cost 5377 B of the
  // ESP32 Arduino loopTask's 8192 — 66% of the stack in one function at the OLD cap of 32 — and
  // raising the cap to 64 would have overflowed it outright. ManagementResponder already hoisted
  // its own reply scratch for exactly this reason (see the note on its reply_ member). With this,
  // the deepest path (duplicate/rename) is ~2.7 KB and, more importantly, **stack no longer scales
  // with kMaxListed at all** — raising the cap again costs .bss, never headroom.
  //
  // SAFETY: one static instance, shared by every store (both modes). That is sound only because a
  // single loop() owns the link and the stores on each MCU — the same assumption TF_Config.h states
  // for dropping the TinyFrame TX mutex. There is no ISR or second task in this path. If that ever
  // changes, these must become per-instance members or acquire a lock.
  //
  // LIFETIMES: the fields are separate precisely so that no live value is clobbered by a nested
  // call. `probe` is only ever a read-only decode target whose value is dead before the next call
  // — which is what lets save() reuse it for the stock read-back and then let nextUseSeq()
  // overwrite it. `stamped` is only ever the outgoing copy handed to writeBlob, so it survives the
  // nextUseSeq() call that fills in its recency field. duplicate()/rename() are the one exception
  // and keep their carried profile on the STACK, because it stays live across save(), which owns
  // both scratch profiles. Re-read this before reusing a field for anything new.
  struct Scratch {
    ProfileEntry names[kMaxListed + 1]; // storage_.list() dest; +1 for the index file itself
    Summary rows[kMaxListed];           // the fully-sorted library, sliced into a window by list()
    oven_Profile probe;                 // read-only decode target; value dead before the next call
    oven_Profile stamped;               // the outgoing copy for writeBlob (save/seedStock/touch)
    uint8_t blob[kBlobCap];             // on-flash bytes, in either direction (loadBlob/writeBlob)
    uint8_t index[kIndexCap];           // the serialized index file (see writeIndex)
  };
  static Scratch &scratch() {
    static Scratch s;
    return s;
  }

  // --- The index file (design.md §23) -----------------------------------------------------------
  //
  // ONE file per mode holding every list row, so building a list is one filesystem open instead of
  // one per profile. That is the whole point, and it comes straight from measurement: on hardware a
  // per-profile LittleFS open costs ~20 ms against ~1.25 ms to nanopb-decode what it contains, so
  // the file count — not the parsing — is what a library walk actually pays for. (An earlier
  // attempt denormalized the same fields into each profile's own header instead; it removed the
  // decode and bought 3%, because it still opened every file.)
  //
  // A record carries exactly what a row needs and what the two sort orders need: `name` is the
  // Alpha key and the MRU tie-break, `use_seq` is the MRU key. Nothing here is authoritative — it
  // is a cache of facts derivable from the profiles themselves, so a missing or corrupt index costs
  // a rebuild, never data. Serialize scratch().rows[0..n) into scratch().index and persist it.
  // Best-effort: a failed write leaves the index stale, and the next read notices and rebuilds.
  bool writeIndex(size_t n) const {
    Scratch &s = scratch();
    if (n > kMaxListed) {
      n = kMaxListed;
    }
    put32(s.index, kIndexMagic);
    put16(s.index + 4, kIndexVersion);
    // The mode is in here for the same reason the body carries it (§7 never-mixed): the index is
    // read WITHOUT decoding any profile, so without this it would be the one path into a list that
    // skips the guard entirely — and two stores sharing a directory would each serve the other's
    // rows. Cheap to record, and it makes a wrong-mode index a rebuild rather than a leak.
    s.index[6] = static_cast<uint8_t>(protocol::wireEnum(mode_) & 0xFF);
    s.index[7] = 0;
    put16(s.index + 8, static_cast<uint16_t>(n));
    for (size_t i = 0; i < n; ++i) {
      uint8_t *r = s.index + kIndexHeaderLen + i * kIndexRecLen;
      std::memset(r, 0, kIndexRecLen);
      std::strncpy(reinterpret_cast<char *>(r), s.rows[i].name, kProfileNameCap - 1);
      r[kProfileNameCap] = s.rows[i].stock ? 1 : 0;
      putFloat(r + kProfileNameCap + 1, s.rows[i].facts.peak_c);
      put32(r + kProfileNameCap + 5, s.rows[i].facts.total_s);
      put32(r + kProfileNameCap + 9, s.rows[i].use_seq);
    }
    return storage_.write(kIndexName, s.index, kIndexHeaderLen + n * kIndexRecLen);
  }

  // Load the index into scratch().rows, UNSORTED. Returns SIZE_MAX if there is no usable index, so
  // "empty library" (0) stays distinguishable from "no index".
  // `crossCheck` compares the record count against the directory. Correct on the READ path, where a
  // disagreement means something changed behind the store's back and a short list would look like
  // data loss. WRONG on the write path: writeBlob() has just added a file the index does not have
  // yet, so a cross-check there reports stale on every single write and sends each one through a
  // full rebuild — which is how the O(n-per-write) walk this index exists to remove crept straight
  // back in (boot 802 ms -> 2797 ms on hardware before this parameter existed).
  size_t readIndex(bool crossCheck) const {
    Scratch &s = scratch();
    const size_t n = storage_.read(kIndexName, s.index, kIndexCap);
    if (n < kIndexHeaderLen || get32(s.index) != kIndexMagic) {
      return SIZE_MAX;
    }
    const uint16_t version =
        static_cast<uint16_t>(s.index[4]) | (static_cast<uint16_t>(s.index[5]) << 8);
    if (static_cast<int32_t>(s.index[6]) != static_cast<int32_t>(mode_)) {
      return SIZE_MAX; // another mode's index (§7) — rebuild ours rather than serve theirs
    }
    const size_t count = static_cast<size_t>(s.index[8]) | (static_cast<size_t>(s.index[9]) << 8);
    // The length must match the count exactly — a truncated index would otherwise be read as a
    // short library, silently hiding profiles rather than triggering a rebuild.
    if (version != kIndexVersion || count > kMaxListed ||
        n != kIndexHeaderLen + count * kIndexRecLen) {
      return SIZE_MAX;
    }
    for (size_t i = 0; i < count; ++i) {
      const uint8_t *r = s.index + kIndexHeaderLen + i * kIndexRecLen;
      std::memcpy(s.rows[i].name, r, kProfileNameCap - 1);
      s.rows[i].name[kProfileNameCap - 1] = '\0';
      s.rows[i].stock = r[kProfileNameCap] != 0;
      s.rows[i].facts.peak_c = getFloat(r + kProfileNameCap + 1);
      s.rows[i].facts.total_s = get32(r + kProfileNameCap + 5);
      s.rows[i].use_seq = get32(r + kProfileNameCap + 9);
    }
    // Cross-check the count against the directory. storage_.list() enumerates NAMES — one directory
    // traversal, no per-file opens — so this is cheap next to what the index saves, and it is the
    // only thing standing between "profiles changed behind the store's back" and an index that is
    // well-formed, self-consistent and quietly short. A library that hides a profile with no error
    // anywhere is indistinguishable from data loss, so err toward rebuilding.
    if (crossCheck) {
      const size_t listed = storage_.list(s.names, kMaxListed + 1);
      size_t onDisk = 0;
      for (size_t i = 0; i < listed && i < kMaxListed + 1; ++i) {
        if (std::strcmp(s.names[i].name, kIndexName) != 0) {
          ++onDisk; // count profiles, rather than assuming the index file is present to subtract
        }
      }
      if (onDisk != count) {
        return SIZE_MAX;
      }
    }
    return count;
  }

  // The slow path: walk every profile, fill scratch().rows, and persist the result as the index.
  // Runs when the index is absent or unusable — a fresh library, an interrupted write, or a
  // filesystem that reformatted under us.
  size_t rebuildIndex() const {
    Scratch &s = scratch();
    size_t total = storage_.list(s.names, kMaxListed + 1); // +1: the index file is in there too
    if (total > kMaxListed + 1) {
      total = kMaxListed + 1;
    }
    size_t m = 0;
    for (size_t i = 0; i < total && m < kMaxListed; ++i) {
      if (std::strcmp(s.names[i].name, kIndexName) == 0) {
        continue; // the index itself is not a profile
      }
      if (!loadBlob(s.names[i].name, s.probe)) {
        continue; // corrupt / wrong-version / other-mode -> not part of this library
      }
      fillRow(s.rows[m], s.names[i].name, s.probe.stock, facts(s.probe), s.probe.use_seq);
      ++m;
    }
    writeIndex(m);
    return m;
  }

  static void fillRow(Summary &out, const char *name, bool stock, const ProfileFacts &f,
                      uint32_t use_seq) {
    std::strncpy(out.name, name, kProfileNameCap - 1);
    out.name[kProfileNameCap - 1] = '\0';
    out.stock = stock;
    out.facts = f;
    out.use_seq = use_seq;
  }

  // Fill scratch().rows from the index (rebuilding it if need be) and order them per `sort`.
  size_t collectSorted(SortMode sort) const {
    size_t m = readIndex(/*crossCheck=*/true);
    if (m == SIZE_MAX) {
      m = rebuildIndex();
    }
    sortRows(scratch().rows, m, sort);
    return m;
  }

  // Re-derive this profile's row and fold it into the index, without touching any other profile.
  // `removed` drops the row instead. Called from every mutation, so the index tracks the library
  // for one read + one write rather than a full walk.
  void indexUpsert(const oven_Profile &p, bool removed) const {
    Scratch &s = scratch();
    size_t m =
        readIndex(/*crossCheck=*/false); // see readIndex: the count is meant to disagree here
    if (m == SIZE_MAX) {
      rebuildIndex(); // no usable index at all: a full rebuild already reflects this write
      return;
    }
    size_t at = m;
    for (size_t i = 0; i < m; ++i) {
      if (std::strcmp(s.rows[i].name, p.name) == 0) {
        at = i;
        break;
      }
    }
    if (removed) {
      if (at < m) {
        for (size_t i = at; i + 1 < m; ++i) {
          s.rows[i] = s.rows[i + 1];
        }
        --m;
      }
    } else if (at < m) {
      fillRow(s.rows[at], p.name, p.stock, facts(p), p.use_seq);
    } else if (m < kMaxListed) {
      fillRow(s.rows[m], p.name, p.stock, facts(p), p.use_seq);
      ++m;
    }
    writeIndex(m);
  }

  // Drop `name` from the index (rename removes the old key; remove() drops the row).
  void indexRemove(const char *name) const {
    oven_Profile key = oven_Profile_init_zero;
    std::strncpy(key.name, name, sizeof(key.name) - 1);
    indexUpsert(key, /*removed=*/true);
  }

  static void put16(uint8_t *p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
  }
  static void put32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
      p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
    }
  }
  static uint32_t get32(const uint8_t *p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      v |= static_cast<uint32_t>(p[i]) << (8 * i);
    }
    return v;
  }
  // float via its bit pattern — memcpy, not a cast, so there is no aliasing UB and no assumption
  // about float alignment inside the blob.
  static void putFloat(uint8_t *p, float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    put32(p, bits);
  }
  static float getFloat(const uint8_t *p) {
    const uint32_t bits = get32(p);
    float f = 0.0F;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
  }

  static void writeHeader(uint8_t *buf) {
    put32(buf, kMagic);
    put16(buf + 4, kVersion);
  }

  static bool checkHeader(const uint8_t *buf, size_t len) {
    return len >= kHeaderLen && get32(buf) == kMagic &&
           (static_cast<uint16_t>(buf[4]) | (static_cast<uint16_t>(buf[5]) << 8)) == kVersion;
  }

  // Decode + mode-check a body out of a buffer that has already been read and header-parsed.
  // Split out so the v1 summary fallback can reuse the bytes it already has instead of re-reading.
  bool decodeBody(const uint8_t *buf, size_t n, oven_Profile &out) const {
    out = oven_Profile_init_zero;
    if (!protocol::decode(oven_Profile_fields, &out, buf + kHeaderLen, n - kHeaderLen)) {
      return false;
    }
    // The §7 never-mixed guard at the store: an out-of-range mode byte or the other mode's profile
    // (a cross-mode upload landing in the wrong dir) is not part of this library. Checked against
    // the BODY, never the header — this is the path a run is fed from.
    return protocol::wireEnum(out.mode) == static_cast<int32_t>(mode_);
  }

  bool loadBlob(const char *name, oven_Profile &out) const {
    uint8_t *buf = scratch().blob; // ~1.5 KB — off the stack, see Scratch
    const size_t n = storage_.read(name, buf, kBlobCap);
    if (!checkHeader(buf, n)) {
      return false;
    }
    return decodeBody(buf, n, out);
  }

  // The next recency stamp: one past the highest use_seq currently stored in this mode's library
  // (1 for an empty library). Because recency is relative, deriving it from the stored max — rather
  // than a separately persisted counter — keeps the whole feature self-contained in the store: the
  // order among surviving profiles is preserved, and there is no extra persistent state to corrupt.
  // O(n) over n <= kMaxListed blobs, only on a save/touch (rare, user-driven). Wraps at 2^32, which
  // is astronomically far off; the comparator tolerates equal/rolled values.
  // Reuses scratch().names — safe because this is never called from inside collectSorted(), the
  // only other user, and callers (save/seedStock/touch) hold their live value in `stamped`.
  uint32_t nextUseSeq() const {
    // Off the index: one file read instead of one per profile. This runs on every save() and on
    // the run-start touch(), so it was the write path's share of the same per-file cost the index
    // exists to remove.
    // Cross-checked: this runs before the write, so the index and the directory should agree.
    size_t m = readIndex(/*crossCheck=*/true);
    if (m == SIZE_MAX) {
      m = rebuildIndex();
    }
    uint32_t mx = 0;
    for (size_t i = 0; i < m; ++i) {
      if (scratch().rows[i].use_seq > mx) {
        mx = scratch().rows[i].use_seq;
      }
    }
    return mx + 1U;
  }

  bool writeBlob(const oven_Profile &p) {
    // scratch().blob, not a stack buffer — and never aliased with `p`, which lives in
    // scratch().stamped or a caller's frame.
    uint8_t *buf = scratch().blob;
    size_t len = 0;
    if (!encodeBlob(p, buf, kBlobCap, len)) {
      return false;
    }
    if (!storage_.write(p.name, buf, len)) {
      return false;
    }
    // The index is a cache of exactly these facts, so it is refreshed with the profile rather than
    // left to be noticed as stale later. Note the ordering: the profile is authoritative and is
    // written first, so an interrupted write can only ever cost a rebuild.
    indexUpsert(p, /*removed=*/false);
    return true;
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
