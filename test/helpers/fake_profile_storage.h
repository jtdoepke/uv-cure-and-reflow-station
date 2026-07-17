// In-memory fake for IProfileStorage — injected into ProfileStore under native_logic_cyd so the
// profile-library round-trips are tested with no LittleFS. Header-only, shared via
// `#include "helpers/fake_profile_storage.h"` (mirrors fake_settings_storage.h / fake_clock.h).
//
// Tests can drive the store through it, or poke `entries` directly to model a stock seed, a
// corrupt/foreign blob, or a cross-mode blob that landed in the wrong dir (a bad WiFi upload).
#pragma once

#include <cstring>
#include <string>
#include <vector>

#include "IProfileStorage.h"

struct FakeProfileStorage : IProfileStorage {
  struct Entry {
    std::string name;
    std::vector<uint8_t> blob;
  };
  std::vector<Entry> entries;
  int writeCalls = 0;
  int removeCalls = 0;

  Entry *find(const char *name) {
    for (auto &e : entries) {
      if (e.name == name) {
        return &e;
      }
    }
    return nullptr;
  }

  // Convenience for tests: drop a raw blob under a name (e.g. a hand-forged stock/corrupt blob).
  void put(const std::string &name, const std::vector<uint8_t> &blob) {
    if (Entry *e = find(name.c_str())) {
      e->blob = blob;
    } else {
      entries.push_back(Entry{name, blob});
    }
  }

  size_t list(ProfileEntry *out, size_t cap) override {
    size_t i = 0;
    for (auto &e : entries) {
      if (i < cap) {
        std::strncpy(out[i].name, e.name.c_str(), kProfileNameCap - 1);
        out[i].name[kProfileNameCap - 1] = '\0';
      }
      ++i;
    }
    return i;
  }

  size_t read(const char *name, uint8_t *buf, size_t cap) override {
    Entry *e = find(name);
    if (!e || e->blob.size() > cap) {
      return 0;
    }
    std::memcpy(buf, e->blob.data(), e->blob.size());
    return e->blob.size();
  }

  bool write(const char *name, const uint8_t *buf, size_t len) override {
    ++writeCalls;
    put(name, std::vector<uint8_t>(buf, buf + len));
    return true;
  }

  bool remove(const char *name) override {
    for (auto it = entries.begin(); it != entries.end(); ++it) {
      if (it->name == name) {
        entries.erase(it);
        ++removeCalls;
        return true;
      }
    }
    return false;
  }
};
