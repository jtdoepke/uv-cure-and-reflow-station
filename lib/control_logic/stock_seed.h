// stock_seed.h — install the factory profile set from the firmware's own compiled-in table
// (design.md §7, §23; backlog S5).
//
// §23 says the seeded defaults are read-only "so the factory references can't be lost". That was
// only half true: delete is refused, but the sole source was a SEPARATE `pio run -t uploadfs`, and
// the controller mounts LittleFS with formatOnFail=true. So a plain `-t upload` left the §23 empty
// state, and a corrupt filesystem reformatted the whole library away — stock included — with no
// field recovery. A promise that survives only while the filesystem does is not the promise §23
// makes. stock_profiles.h is the copy the filesystem cannot take with it; this is what installs it.
//
// Two callers, one function, distinguished only by `overwrite`:
//   - BOOT (overwrite = false): fill in anything missing. Idempotent, so a populated board does
//     nothing and a freshly formatted one repopulates itself.
//   - RESTORE (overwrite = true, §24's "Restore stock profiles"): rewrite every stock entry,
//     repairing one that was corrupted.
//
// Neither ever clobbers a USER profile holding a stock name — see ProfileStore::seedStock, which
// enforces that and is where the "only from the compiled table, never from the wire" rule lives.
//
// Pure C++ over nanopb: no Arduino, no LittleFS — host-tested under native_control against a
// FakeProfileStorage, like the store it drives.
#pragma once

#include <cstddef>

#include "codec.h"
#include "oven.pb.h"
#include "profile_library.h"
#include "stock_profiles.h"

namespace control {

// Per-outcome counts rather than a bool, because the interesting cases are not failures and a
// caller that collapsed them would report "restored" for a library it did not touch. §24 shows
// these back to the operator.
struct SeedReport {
  size_t written = 0;   // installed (absent, or overwritten on a restore)
  size_t present = 0;   // already there and not overwritten
  size_t userOwned = 0; // a user profile holds the name; left alone
  size_t failed = 0;    // undecodable table entry, or a write failure

  size_t considered() const { return written + present + userOwned + failed; }
  bool ok() const { return failed == 0; }
};

// Install the compiled-in stock entries whose mode is this store's. Entries for the other mode are
// skipped, not failed — one store is one mode's library (§7 "never mixed").
inline SeedReport seedStockProfiles(ProfileStore &store, bool overwrite) {
  SeedReport r;
  for (size_t i = 0; i < stock::kCount; ++i) {
    const stock::Entry &e = stock::kEntries[i];
    if (e.mode != store.mode()) {
      continue;
    }
    oven_Profile p = oven_Profile_init_zero;
    // A table entry that will not decode is a build-time mistake, not an attack — but count it
    // rather than assert: a controller that refuses to boot over a bad seed blob is worse than one
    // that comes up with a short library and says so.
    if (!protocol::decode(oven_Profile_fields, &p, e.body, e.len)) {
      ++r.failed;
      continue;
    }
    switch (store.seedStock(p, overwrite)) {
    case ProfileStore::SeedOutcome::Written:
      ++r.written;
      break;
    case ProfileStore::SeedOutcome::Present:
      ++r.present;
      break;
    case ProfileStore::SeedOutcome::UserOwned:
      ++r.userOwned;
      break;
    case ProfileStore::SeedOutcome::Failed:
      ++r.failed;
      break;
    }
  }
  return r;
}

} // namespace control
