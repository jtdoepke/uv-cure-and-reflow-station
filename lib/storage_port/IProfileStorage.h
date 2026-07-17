// IProfileStorage — the CYD's per-mode profile-library storage port (design.md §7, §23).
//
// ProfileStore (lib/app_logic) owns the typed Phase[] model + serialization + validation; this
// port is deliberately just "keyed blob CRUD in one namespace" — list / read / write / remove a
// blob by name — so the store decides the byte layout and the adapter stays dumb. Per-mode
// separation (§7 "never mixed": /profiles/cure/, /profiles/reflow/) is achieved by constructing
// one ProfileStore over one adapter bound to that mode's directory; this port itself is
// mode-agnostic. It is the multi-entry sibling of the single-blob ISettingsStorage (B5): a
// LittleFS adapter on device (src_cyd/), an in-memory fake on host (test/helpers/).
//
// Keep this header free of <Arduino.h> so it stays native-compilable (mirrors ISettingsStorage.h
// and the lib/control_port / lib/display_port idiom).
#pragma once

#include <cstddef>
#include <cstdint>

// Max profile name length including the NUL terminator. A profile's name is also its storage key
// (the LittleFS filename stem, §7), so this bounds both — 31 usable chars, ample for the §23 names
// ("LF-245", "LF-245 copy"). ProfileStore validates a name against this before it reaches an
// adapter, so no untrusted (PC/WiFi-uploaded) name can overrun a fixed buffer here.
inline constexpr size_t kProfileNameCap = 32;

// One enumerated entry: the profile's name (its key). Fixed buffer so list() needs no heap.
struct ProfileEntry {
  char name[kProfileNameCap];
};

struct IProfileStorage {
  virtual ~IProfileStorage() = default;

  // Enumerate stored entry names into `out` (writing at most `cap`) and return the total number of
  // entries present (which may exceed `cap`; only `cap` are written). Order is unspecified — the
  // store sorts. Names are always NUL-terminated within kProfileNameCap.
  virtual size_t list(ProfileEntry *out, size_t cap) = 0;

  // Copy the named blob into `buf` (at most `cap` bytes) and return the bytes read. Returns 0 when
  // the name is absent or the blob doesn't fit `cap` — the store treats either as "not found".
  virtual size_t read(const char *name, uint8_t *buf, size_t cap) = 0;

  // Persist `len` bytes under `name`, replacing any previous blob for that name. Returns false on
  // write failure.
  virtual bool write(const char *name, const uint8_t *buf, size_t len) = 0;

  // Remove the named blob. Returns false when the name is absent or on failure.
  virtual bool remove(const char *name) = 0;
};
