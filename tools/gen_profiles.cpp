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
#include <initializer_list>
#include <string>

#include "IProfileStorage.h"
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

// Author one profile (its mode is stamped by the store, not trusted from here) and write it,
// removing any stale blob first so a re-run cannot be refused as a stock-overwrite (§23).
bool emit(control::ProfileStore &store, const char *name, bool stock,
          std::initializer_list<oven_Phase> phases) {
  oven_Profile p = oven_Profile_init_zero;
  p.mode = store.mode();
  std::strncpy(p.name, name, sizeof(p.name) - 1);
  p.stock = stock;
  for (const oven_Phase &ph : phases) {
    p.phases[p.phases_count++] = ph;
  }
  store.remove(name);
  const bool ok = store.save(p);
  std::printf("  %-8s %s  (%u phases)%s\n", name, ok ? "written" : "FAILED",
              (unsigned)p.phases_count, stock ? "  [stock]" : "");
  return ok;
}

} // namespace

int main(int argc, char **argv) {
  const std::string root = argc > 1 ? argv[1] : "data/profiles";
  ::mkdir(root.c_str(), 0755);

  FileProfileStorage reflow_fs(root + "/reflow");
  FileProfileStorage cure_fs(root + "/cure");
  control::ProfileStore reflow(reflow_fs, oven_Mode_MODE_REFLOW);
  control::ProfileStore cure(cure_fs, oven_Mode_MODE_CURE);

  std::printf("Writing stock fixtures under %s/ (controller PRO2 format)\n", root.c_str());
  bool ok = true;
  // LF-245 — a leaded reflow profile (245 C peak), with an authored final cool phase.
  ok &= emit(reflow, "LF-245", /*stock=*/false,
             {phase("Preheat", 150.0F, 90.0F, 90.0F), phase("Soak", 180.0F, 60.0F, 60.0F),
              phase("Reflow", 245.0F, 35.0F, 30.0F), phase("Cool", 50.0F, 0.0F, 0.0F)});
  // SAC305 — a lead-free reflow profile, flagged stock to demo the §23 read-only gating.
  ok &= emit(reflow, "SAC305", /*stock=*/true,
             {phase("Soak", 165.0F, 100.0F, 90.0F), phase("Reflow", 249.0F, 40.0F, 30.0F),
              phase("Cool", 50.0F, 0.0F, 0.0F)});
  // Resin-A — a UV-cure profile: a dosed cure phase (turntable + UV) then a cool-down.
  ok &= emit(cure, "Resin-A", /*stock=*/false,
             {phase("Cure", 60.0F, 0.0F, 0.0F, /*exp=*/45.0F, /*uv=*/true, /*motor=*/true),
              phase("Cool", 40.0F, 0.0F, 0.0F)});

  std::printf(ok ? "OK\n" : "SOME WRITES FAILED\n");
  return ok ? 0 : 1;
}
