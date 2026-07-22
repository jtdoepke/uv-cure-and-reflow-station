// gen_profiles.cpp — regenerate the committed stock profile fixtures (data/profiles/**/*.bin).
//
// Host-only, NOT part of the firmware build. The fixtures are raw control::ProfileStore blobs
// (a versioned header + a nanopb-encoded oven_Profile, §7) that `pio run -t uploadfs` drops onto
// the CONTROLLER (Wave R2 of the §2 "CYD is a UI remote" split, 2026-07-17 — the profile library
// moved off the CYD) so a fresh board boots with a browsable library. Any change to the wire
// Phase/Profile schema changes the blob, so rerun this to rebuild the fixtures format-correct by
// construction (through the real store) rather than by hand.
//
// Build + run from the project root (nanopb codegen runs in the env):
//   pio run -e gen_profiles
//   .pio/build/gen_profiles/program data/profiles
//
// Superseded the pre-R2 version, which wrote the CYD's old memcpy "PRO1" blobs; the controller's
// store speaks the nanopb "PRO2" format, so the old fixtures would be rejected on load.

#include <sys/stat.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "IProfileStorage.h"
#include "codec.h" // protocol::encode — the generated header carries nanopb bodies
#include "oven.pb.h"
#include "profile_library.h"

namespace {

// Minimal file-backed IProfileStorage: one directory per mode, "<dir>/<name>.bin" — the same
// convention as the firmware LittleFsProfileStorage adapter (src_control/).
class FileProfileStorage : public IProfileStorage {
public:
  explicit FileProfileStorage(std::string dir) : dir_(std::move(dir)) {
    ::mkdir(dir_.c_str(), 0755); // best-effort; ignore "already exists"
  }
  size_t list(ProfileEntry *, size_t) override { return 0; } // unused by save()
  size_t read(const char *name, uint8_t *buf, size_t cap) override {
    FILE *f = std::fopen(path(name).c_str(), "rb");
    if (f == nullptr) {
      return 0;
    }
    const size_t n = std::fread(buf, 1, cap, f);
    std::fclose(f);
    return n;
  }
  bool write(const char *name, const uint8_t *buf, size_t len) override {
    FILE *f = std::fopen(path(name).c_str(), "wb");
    if (f == nullptr) {
      return false;
    }
    const size_t n = std::fwrite(buf, 1, len, f);
    std::fclose(f);
    return n == len;
  }
  bool remove(const char *name) override { return std::remove(path(name).c_str()) == 0; }

private:
  std::string path(const char *name) const { return dir_ + "/" + name + ".bin"; }
  std::string dir_;
};

// A profile name mapped onto a C identifier for the generated array symbol ("LF-245" -> "LF_245").
std::string identifier(const char *name) {
  std::string id;
  for (const char *c = name; *c != '\0'; ++c) {
    const bool alnum =
        (*c >= '0' && *c <= '9') || (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z');
    id += alnum ? *c : '_';
  }
  return id;
}

oven_Phase phase(const char *name, float targetC, float ramp, float hold, float exp = 0.0F,
                 bool uv = false, bool motor = false) {
  oven_Phase p = oven_Phase_init_zero;
  std::strncpy(p.name, name, sizeof(p.name) - 1);
  p.target_c = targetC;
  p.ramp_s = ramp;
  p.hold_s = hold;
  p.exposure_per_surface = exp;
  p.uv = uv;
  p.motor = motor;
  p.fan_mode = oven_FanMode_FAN_MODE_AUTO;
  return p;
}

// One authored profile. THE table below is the single source for both outputs — the `data/`
// fixtures and the compiled-in seed header — so the two can never disagree about what "stock"
// means or what a stock profile contains.
struct Def {
  const char *name;
  oven_Mode mode;
  bool stock;
  std::vector<oven_Phase> phases;
};

std::vector<Def> definitions() {
  return {
      // LF-245 — a leaded reflow profile (245 C peak), with an authored final cool phase.
      {"LF-245",
       oven_Mode_MODE_REFLOW,
       /*stock=*/false,
       {phase("Preheat", 150.0F, 90.0F, 90.0F), phase("Soak", 180.0F, 60.0F, 60.0F),
        phase("Reflow", 245.0F, 35.0F, 30.0F), phase("Cool", 50.0F, 0.0F, 0.0F)}},
      // SAC305 — a lead-free reflow profile, flagged stock to demo the §23 read-only gating.
      {"SAC305",
       oven_Mode_MODE_REFLOW,
       /*stock=*/true,
       {phase("Soak", 165.0F, 100.0F, 90.0F), phase("Reflow", 249.0F, 40.0F, 30.0F),
        phase("Cool", 50.0F, 0.0F, 0.0F)}},
      // Resin-A — a UV-cure profile: a dosed cure phase (turntable + UV) then a cool-down.
      {"Resin-A",
       oven_Mode_MODE_CURE,
       /*stock=*/false,
       {phase("Cure", 60.0F, 0.0F, 0.0F, /*exp=*/45.0F, /*uv=*/true, /*motor=*/true),
        phase("Cool", 40.0F, 0.0F, 0.0F)}},
  };
}

oven_Profile toProfile(const Def &d) {
  oven_Profile p = oven_Profile_init_zero;
  p.mode = d.mode;
  std::strncpy(p.name, d.name, sizeof(p.name) - 1);
  p.stock = d.stock;
  for (const oven_Phase &ph : d.phases) {
    p.phases[p.phases_count++] = ph;
  }
  return p;
}

// Write one fixture. The store stamps the mode; it is not trusted from here.
//
// A STOCK entry cannot go through remove()+save(): §23 makes both refuse a stock profile, so the
// old code's "remove any stale blob first so a re-run cannot be refused as a stock-overwrite"
// did not do that at all — remove() returned false and save() then refused the overwrite. It
// only ever worked on a fixture that did not yet exist, so re-running the generator over a
// committed stock fixture always failed (found 2026-07-21, once seedStock existed to fix it).
// seedStock(overwrite) IS this operation: install a factory profile over the stock one there.
bool emit(control::ProfileStore &store, const Def &d) {
  const oven_Profile p = toProfile(d);
  bool ok;
  if (d.stock) {
    ok = store.seedStock(p, /*overwrite=*/true) == control::ProfileStore::SeedOutcome::Written;
  } else {
    store.remove(d.name);
    ok = store.save(p);
  }
  std::printf("  %-8s %s  (%u phases)%s\n", d.name, ok ? "written" : "FAILED",
              (unsigned)p.phases_count, d.stock ? "  [stock]" : "");
  return ok;
}

// Emit the compiled-in seed table (lib/control_logic/stock_profiles.h).
//
// Only the STOCK entries go in: the non-stock fixtures exist to demo the §23 gating and to give a
// dev board something to browse, and seeding a user-editable profile into every fresh flash would
// keep resurrecting one the operator deleted on purpose.
//
// Bodies are the nanopb-encoded oven_Profile, NOT the store's on-flash blob: the store owns its
// own header and stamps mode/use_seq at seed time, so storing a pre-wrapped blob would freeze a
// header this generator does not own. Encoded rather than a C struct literal because an
// oven_Profile carries a 32-slot static phase array (~2 KB each) and would have to track field
// order by hand; the encoding is compact and schema-versioned for free.
bool emitHeader(const std::string &path, const std::vector<Def> &defs) {
  FILE *f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) {
    std::printf("  FAILED to open %s\n", path.c_str());
    return false;
  }
  std::fprintf(f, R"(// GENERATED by tools/gen_profiles.cpp -- do not edit.
//
// The stock profile set (design.md §7/§23), compiled into the controller firmware as
// nanopb-encoded oven_Profile bodies. Regenerate with:
//   pio run -e gen_profiles && .pio/build/gen_profiles/program data/profiles
//
// Why this exists at all, rather than `uploadfs` alone: `data/profiles/**` reaches the board only
// via a SEPARATE `pio run -t uploadfs`, and the controller mounts LittleFS with
// formatOnFail=true. So a plain `-t upload` leaves the §23 empty state, and a corrupt filesystem
// silently reformats the library away -- stock included -- with no recovery short of a USB
// reflash. §23 promises the factory references "can't be lost"; that promise needs a copy the
// filesystem cannot take with it. Boot seeds any missing entry from here, which also makes
// `uploadfs` optional rather than load-bearing.
//
// This is the ONLY legitimate input to ProfileStore::seedStock() -- see the warning there. It is
// reviewed source compiled into this firmware, never anything that arrived over the wire.
//
// const, so it lives in flash .rodata and costs the controller's DRAM nothing (§6a).
#pragma once

#include <cstddef>
#include <cstdint>

#include "oven.pb.h"

namespace control {
namespace stock {

struct Entry {
  const char *name;
  oven_Mode mode;
  const uint8_t *body; // nanopb-encoded oven_Profile
  size_t len;
};

)");

  size_t emitted = 0;
  for (const Def &d : defs) {
    if (!d.stock) {
      continue;
    }
    const oven_Profile p = toProfile(d);
    uint8_t buf[oven_Profile_size];
    size_t len = 0;
    if (!protocol::encode(oven_Profile_fields, &p, buf, sizeof(buf), len)) {
      std::printf("  FAILED to encode %s\n", d.name);
      std::fclose(f);
      return false;
    }
    std::fprintf(f, "inline constexpr uint8_t k%s[] = {", identifier(d.name).c_str());
    for (size_t i = 0; i < len; ++i) {
      std::fprintf(f, "%s0x%02X,", (i % 12 == 0) ? "\n    " : " ", buf[i]);
    }
    std::fprintf(f, "\n};\n\n");
    ++emitted;
  }

  std::fprintf(f, "inline constexpr Entry kEntries[] = {\n");
  for (const Def &d : defs) {
    if (!d.stock) {
      continue;
    }
    const std::string id = identifier(d.name);
    std::fprintf(f, "    {\"%s\", %s, k%s, sizeof(k%s)},\n", d.name,
                 d.mode == oven_Mode_MODE_CURE ? "oven_Mode_MODE_CURE" : "oven_Mode_MODE_REFLOW",
                 id.c_str(), id.c_str());
  }
  std::fprintf(f, "};\n\ninline constexpr size_t kCount = sizeof(kEntries) / sizeof(kEntries[0]);\n"
                  "\n} // namespace stock\n} // namespace control\n");
  std::fclose(f);
  std::printf("  %s written (%u stock entries)\n", path.c_str(), (unsigned)emitted);
  return true;
}

} // namespace

int main(int argc, char **argv) {
  const std::string root = argc > 1 ? argv[1] : "data/profiles";
  ::mkdir(root.c_str(), 0755);

  FileProfileStorage reflow_fs(root + "/reflow");
  FileProfileStorage cure_fs(root + "/cure");
  control::ProfileStore reflow(reflow_fs, oven_Mode_MODE_REFLOW);
  control::ProfileStore cure(cure_fs, oven_Mode_MODE_CURE);

  const std::vector<Def> defs = definitions();

  std::printf("Writing stock fixtures under %s/ (controller PRO2 format)\n", root.c_str());
  bool ok = true;
  for (const Def &d : defs) {
    ok &= emit(d.mode == oven_Mode_MODE_CURE ? cure : reflow, d);
  }

  std::printf("Writing the compiled-in seed table\n");
  ok &= emitHeader("lib/control_logic/stock_profiles.h", defs);

  std::printf(ok ? "OK\n" : "SOME WRITES FAILED\n");
  return ok ? 0 : 1;
}
