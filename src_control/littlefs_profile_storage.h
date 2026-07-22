// LittleFsProfileStorage — the IProfileStorage firmware adapter (design.md §7). One instance per
// mode directory (e.g. "/profiles/cure"); a profile named "LF-245" is the file "<dir>/LF-245.bin".
// control::ProfileStore owns the blob byte layout, so this stays dumb: keyed blob CRUD over
// LittleFS files.
//
// Firmware-only: it #includes <LittleFS.h> (Arduino/ESP-IDF), so it lives in src_control/ and
// never compiles for the native test targets; the host tests drive the store through
// FakeProfileStorage instead. LittleFS.begin() is mounted once at boot (main.cpp); this adapter
// creates its own mode directory on first write.
//
// Moved from src_cyd/ to src_control/ with the 2026-07-17 "CYD is a UI remote" split (§2/§7,
// Wave R2): the profile library now lives on the controller's flash. The CYD sheds its copy in R3.
#pragma once

#include <Arduino.h>
#include <LittleFS.h>

#include <cstdio>
#include <cstring>

#include "IProfileStorage.h"

class LittleFsProfileStorage : public IProfileStorage {
public:
  explicit LittleFsProfileStorage(const char *dir) : dir_(dir) {}

  size_t list(ProfileEntry *out, size_t cap) override {
    File d = LittleFS.open(dir_);
    if (!d || !d.isDirectory()) {
      return 0; // dir not created yet (no profiles saved) -> empty library
    }
    size_t i = 0;
    for (File f = d.openNextFile(); f; f = d.openNextFile()) {
      if (f.isDirectory()) {
        continue;
      }
      char name[kProfileNameCap];
      if (!stemFromPath(f.name(), name)) {
        continue; // not a "<name>.bin" we own, or the stem doesn't fit
      }
      if (i < cap) {
        std::strncpy(out[i].name, name, kProfileNameCap - 1);
        out[i].name[kProfileNameCap - 1] = '\0';
      }
      ++i;
    }
    return i;
  }

  size_t read(const char *name, uint8_t *buf, size_t cap) override {
    char path[kPathCap];
    if (!pathFor(name, path)) {
      return 0;
    }
    File f = LittleFS.open(path, "r");
    if (!f || f.isDirectory()) {
      return 0;
    }
    const size_t len = f.size();
    if (len == 0 || len > cap) {
      f.close();
      return 0;
    }
    const size_t n = f.read(buf, len);
    f.close();
    return n == len ? n : 0;
  }

  bool write(const char *name, const uint8_t *buf, size_t len) override {
    char path[kPathCap];
    if (!pathFor(name, path)) {
      return false;
    }
    ensureDir(dir_); // first save into this mode's library, or a freshly formatted volume
    File f = LittleFS.open(path, "w");
    if (!f) {
      return false;
    }
    const size_t n = f.write(buf, len);
    f.close();
    return n == len;
  }

  bool remove(const char *name) override {
    char path[kPathCap];
    if (!pathFor(name, path)) {
      return false;
    }
    return LittleFS.remove(path);
  }

private:
  static constexpr size_t kPathCap = 64; // "/profiles/reflow/" + 31-char name + ".bin" + NUL fits
  static constexpr const char *kExt = ".bin";

  // Create `dir` and every parent it needs.
  //
  // **LittleFS.mkdir() is NOT recursive**, and on a freshly formatted volume neither "/profiles"
  // nor "/profiles/<mode>" exists — so a lone mkdir("/profiles/reflow") fails and the open() after
  // it fails too. This went unnoticed because `uploadfs` shipped both directories inside the image,
  // so the only volume the firmware ever wrote to already had them. Found the first time a board
  // booted onto a formatted-empty filesystem (bench, 2026-07-22): the §23 stock seed reported
  // cure=0 reflow=0 with `fopen(/littlefs/profiles/reflow/SAC305.bin) failed` above it.
  //
  // No host test could have caught it — FakeProfileStorage is a keyed map with no directories at
  // all, which is the right shape for the port (the port is "keyed blob CRUD", §7) and precisely
  // why this is adapter-only behaviour that needs hardware to exercise.
  static void ensureDir(const char *dir) {
    char path[kPathCap];
    const size_t len = std::strlen(dir);
    if (len == 0 || len >= kPathCap) {
      return;
    }
    std::memcpy(path, dir, len + 1);
    // Walk the components, creating each prefix in turn. Start at 1: index 0 is the leading '/'.
    for (size_t i = 1; i <= len; ++i) {
      if (path[i] != '/' && path[i] != '\0') {
        continue;
      }
      const char saved = path[i];
      path[i] = '\0';
      if (!LittleFS.exists(path)) {
        LittleFS.mkdir(path);
      }
      path[i] = saved;
    }
  }

  // "<dir>/<name>.bin". Rejects a name that (with the dir + extension) would overrun kPathCap;
  // ProfileStore::validName already bars separators/length, but keep the buffer guard local too.
  bool pathFor(const char *name, char *out) const {
    const int n = std::snprintf(out, kPathCap, "%s/%s%s", dir_, name, kExt);
    return n > 0 && static_cast<size_t>(n) < kPathCap;
  }

  // Extract "<name>" from a ".../<name>.bin" path (or a bare "<name>.bin" — arduino-esp32 versions
  // differ on whether File::name() is a full path or a basename). Returns false without an .bin
  // extension or when the stem doesn't fit kProfileNameCap.
  static bool stemFromPath(const char *path, char *out) {
    const char *slash = std::strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    const size_t blen = std::strlen(base);
    const size_t extlen = std::strlen(kExt);
    if (blen <= extlen || std::strcmp(base + blen - extlen, kExt) != 0) {
      return false;
    }
    const size_t stem = blen - extlen;
    if (stem >= kProfileNameCap) {
      return false;
    }
    std::memcpy(out, base, stem);
    out[stem] = '\0';
    return true;
  }

  const char *dir_;
};
