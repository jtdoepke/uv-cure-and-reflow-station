// gen_profiles.cpp — regenerate the committed demo profile fixtures (data/profiles/**/*.bin).
//
// Host-only, NOT part of the firmware build. The fixtures are raw ProfileStore PersistedBlob bytes
// (§7) that `pio run -t uploadfs` drops onto the device so C4/C5 have a browsable library on real
// hardware. Because the blob embeds the raw Phase[], any change to the Phase layout (e.g. adding
// the per-phase name field) changes the byte layout and the old fixtures stop loading — rerun this
// to rebuild them, format-correct by construction (through ProfileStore::save) rather than by hand.
//
// Build + run from the project root:
//   g++ -std=c++17 -Ilib/app_logic -Ilib/storage_port tools/gen_profiles.cpp -o /tmp/gen_profiles
//   /tmp/gen_profiles data/profiles
//
// It also seeds the deferred §23 stock-seed generator (a reviewable source → blobs); today the
// three profiles are authored inline below.

#include <sys/stat.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "IProfileStorage.h"
#include "phase.h"
#include "profile_store.h"

namespace {

// Minimal file-backed IProfileStorage: one directory per mode, "<dir>/<name>.bin" — the same
// convention as the firmware LittleFsProfileStorage adapter (src_cyd/).
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

Phase phase(const char *name, float targetC, float ramp, float hold, float exp = 0.0f,
            bool uv = false, bool motor = false) {
  Phase p;
  std::strncpy(p.name, name, kPhaseNameCap - 1);
  p.name[kPhaseNameCap - 1] = '\0';
  p.targetC = targetC;
  p.rampSeconds = ramp;
  p.holdSeconds = hold;
  p.exposurePerSurface = exp;
  p.uv = uv;
  p.motor = motor;
  return p;
}

// Author one profile (its mode is stamped by the store, not trusted from here) and write it,
// removing any stale blob first so a re-run cannot be refused as a stock-overwrite (§23).
bool emit(ProfileStore &store, const char *name, bool stock, std::initializer_list<Phase> phases) {
  ProfileStore::StoredProfile p;
  std::strncpy(p.name, name, kProfileNameCap - 1);
  p.name[kProfileNameCap - 1] = '\0';
  p.mode = store.mode();
  p.stock = stock;
  p.phaseCount = 0;
  for (const Phase &ph : phases) {
    p.phases[p.phaseCount++] = ph;
  }
  store.remove(name);
  const bool ok = store.save(p);
  std::printf("  %-8s %s  (%zu phases)%s\n", name, ok ? "written" : "FAILED", p.phaseCount,
              stock ? "  [stock]" : "");
  return ok;
}

} // namespace

int main(int argc, char **argv) {
  const std::string root = argc > 1 ? argv[1] : "data/profiles";
  ::mkdir(root.c_str(), 0755);

  FileProfileStorage reflow_fs(root + "/reflow");
  FileProfileStorage cure_fs(root + "/cure");
  ProfileStore reflow(reflow_fs, RecipeMode::Reflow);
  ProfileStore cure(cure_fs, RecipeMode::Cure);

  std::printf("Writing demo fixtures under %s/\n", root.c_str());
  bool ok = true;
  // LF-245 — a leaded reflow profile (245 C peak), with an authored final cool phase.
  ok &= emit(reflow, "LF-245", /*stock=*/false,
             {phase("Preheat", 150.0f, 90.0f, 90.0f), phase("Soak", 180.0f, 60.0f, 60.0f),
              phase("Reflow", 245.0f, 35.0f, 30.0f), phase("Cool", 50.0f, 0.0f, 0.0f)});
  // SAC305 — a lead-free reflow profile, flagged stock to demo the §23 read-only gating.
  ok &= emit(reflow, "SAC305", /*stock=*/true,
             {phase("Soak", 165.0f, 100.0f, 90.0f), phase("Reflow", 249.0f, 40.0f, 30.0f),
              phase("Cool", 50.0f, 0.0f, 0.0f)});
  // Resin-A — a UV-cure profile: a dosed cure phase (turntable + UV) then a cool-down.
  ok &= emit(cure, "Resin-A", /*stock=*/false,
             {phase("Cure", 60.0f, 0.0f, 0.0f, /*exp=*/45.0f, /*uv=*/true, /*motor=*/true),
              phase("Cool", 40.0f, 0.0f, 0.0f)});

  std::printf(ok ? "OK\n" : "SOME WRITES FAILED\n");
  return ok ? 0 : 1;
}
