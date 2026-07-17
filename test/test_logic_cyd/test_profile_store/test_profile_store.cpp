// native_logic_cyd suite — pure host tests of ProfileStore (backlog B4), the per-mode profile
// library + persistence behind the IProfileStorage port. No LVGL, no Arduino: a FakeProfileStorage
// stands in for LittleFS. Covers the empty library, a full save/load round-trip (every Phase field
// survives), alphabetical listing with stock flags, the §23 stock read-only rule (save-over and
// delete refused), duplicate, name validation, the §7 "never mixed" mode guard, the kMaxPhases
// bound, and graceful rejection of a short / bad-magic / wrong-mode blob (an untrusted upload).
#include <unity.h>

#include <cstdint>
#include <cstring>

#include "helpers/fake_profile_storage.h"
#include "phase.h"
#include "profile_store.h"

using StoredProfile = ProfileStore::StoredProfile;
using Summary = ProfileStore::Summary;

void setUp(void) {}
void tearDown(void) {}

namespace {

// A phase with distinct, recoverable values per field so a round-trip can prove each one persisted.
Phase mkPhase(float t) {
  Phase p;
  p.targetC = t;
  p.rampSeconds = t + 5.0f;
  p.holdSeconds = t + 11.0f;
  p.exposurePerSurface = t + 0.5f;
  p.uv = true;
  p.motor = false;
  p.convFan = FanMode::On;
  return p;
}

StoredProfile mkProfile(const char *name, RecipeMode mode, bool stock, size_t nPhases) {
  StoredProfile p;
  std::strncpy(p.name, name, kProfileNameCap - 1);
  p.mode = mode;
  p.stock = stock;
  p.phaseCount = nPhases;
  for (size_t i = 0; i < nPhases; ++i) {
    p.phases[i] = mkPhase(static_cast<float>(100 + i));
  }
  return p;
}

} // namespace

// --- Empty library ---

void test_empty_store(void) {
  FakeProfileStorage fs;
  ProfileStore store(fs, RecipeMode::Reflow);
  Summary rows[ProfileStore::kMaxListed];
  TEST_ASSERT_EQUAL_UINT(0, store.list(rows, ProfileStore::kMaxListed));
  StoredProfile out;
  TEST_ASSERT_FALSE(store.load("nope", out));
}

// --- Round-trip: every Phase field survives ---

void test_save_load_round_trip(void) {
  FakeProfileStorage fs;
  ProfileStore store(fs, RecipeMode::Cure);
  StoredProfile p = mkProfile("LF-245", RecipeMode::Cure, /*stock=*/false, /*nPhases=*/3);
  TEST_ASSERT_TRUE(store.save(p));
  TEST_ASSERT_EQUAL_INT(1, fs.writeCalls);

  StoredProfile out;
  TEST_ASSERT_TRUE(store.load("LF-245", out));
  TEST_ASSERT_EQUAL_STRING("LF-245", out.name);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(RecipeMode::Cure), static_cast<int>(out.mode));
  TEST_ASSERT_FALSE(out.stock);
  TEST_ASSERT_EQUAL_UINT(3, out.phaseCount);
  for (size_t i = 0; i < out.phaseCount; ++i) {
    const Phase &a = p.phases[i];
    const Phase &b = out.phases[i];
    TEST_ASSERT_EQUAL_FLOAT(a.targetC, b.targetC);
    TEST_ASSERT_EQUAL_FLOAT(a.rampSeconds, b.rampSeconds);
    TEST_ASSERT_EQUAL_FLOAT(a.holdSeconds, b.holdSeconds);
    TEST_ASSERT_EQUAL_FLOAT(a.exposurePerSurface, b.exposurePerSurface);
    TEST_ASSERT_EQUAL_INT(a.uv, b.uv);
    TEST_ASSERT_EQUAL_INT(a.motor, b.motor);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(a.convFan), static_cast<int>(b.convFan));
  }
}

// save() re-stamps the store's own mode, never trusting the caller's field.
void test_save_stamps_store_mode(void) {
  FakeProfileStorage fs;
  ProfileStore store(fs, RecipeMode::Reflow);
  StoredProfile p = mkProfile("X", RecipeMode::Cure, false, 1); // caller lies: mode Cure
  p.phases[0].uv = false;                                       // keep it plausible for reflow
  TEST_ASSERT_TRUE(store.save(p));
  StoredProfile out;
  TEST_ASSERT_TRUE(store.load("X", out));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(RecipeMode::Reflow), static_cast<int>(out.mode));
}

// --- Listing: alphabetical, with stock flags, skipping foreign entries ---

void test_list_alphabetical_with_stock(void) {
  FakeProfileStorage fs;
  ProfileStore store(fs, RecipeMode::Reflow);
  StoredProfile a = mkProfile("MyBoard", RecipeMode::Reflow, false, 2);
  a.phases[0].uv = false;
  a.phases[1].uv = false;
  StoredProfile b = mkProfile("LF-245", RecipeMode::Reflow, false, 1);
  b.phases[0].uv = false;
  StoredProfile c = mkProfile("SAC305", RecipeMode::Reflow, false, 1);
  c.phases[0].uv = false;
  TEST_ASSERT_TRUE(store.save(a));
  TEST_ASSERT_TRUE(store.save(b));
  TEST_ASSERT_TRUE(store.save(c));

  Summary rows[ProfileStore::kMaxListed];
  const size_t n = store.list(rows, ProfileStore::kMaxListed);
  TEST_ASSERT_EQUAL_UINT(3, n);
  TEST_ASSERT_EQUAL_STRING("LF-245", rows[0].name); // alphabetical
  TEST_ASSERT_EQUAL_STRING("MyBoard", rows[1].name);
  TEST_ASSERT_EQUAL_STRING("SAC305", rows[2].name);
  TEST_ASSERT_EQUAL_UINT(2, rows[1].phaseCount);
  for (size_t i = 0; i < n; ++i) {
    TEST_ASSERT_FALSE(rows[i].stock);
  }
}

// --- Stock read-only (§23): save-over and delete refused ---

void test_stock_is_read_only(void) {
  FakeProfileStorage fs;
  ProfileStore store(fs, RecipeMode::Reflow);
  StoredProfile stock = mkProfile("LF-lowpk", RecipeMode::Reflow, /*stock=*/true, 1);
  stock.phases[0].uv = false;
  TEST_ASSERT_TRUE(store.save(stock)); // initial seed write is allowed (no prior entry)

  // Overwriting the stock name is refused, and the stored bytes are untouched.
  StoredProfile edit = mkProfile("LF-lowpk", RecipeMode::Reflow, false, 2);
  edit.phases[0].uv = false;
  edit.phases[1].uv = false;
  const int writesBefore = fs.writeCalls;
  TEST_ASSERT_FALSE(store.save(edit));
  TEST_ASSERT_EQUAL_INT(writesBefore, fs.writeCalls);
  StoredProfile out;
  TEST_ASSERT_TRUE(store.load("LF-lowpk", out));
  TEST_ASSERT_EQUAL_UINT(1, out.phaseCount); // still the stock version

  // Delete is refused for stock.
  TEST_ASSERT_FALSE(store.remove("LF-lowpk"));
  TEST_ASSERT_TRUE(store.load("LF-lowpk", out));
}

// --- Duplicate produces a user copy; refuses collisions and a missing source ---

void test_duplicate(void) {
  FakeProfileStorage fs;
  ProfileStore store(fs, RecipeMode::Reflow);
  StoredProfile stock = mkProfile("LF-245", RecipeMode::Reflow, /*stock=*/true, 2);
  stock.phases[0].uv = false;
  stock.phases[1].uv = false;
  TEST_ASSERT_TRUE(store.save(stock));

  TEST_ASSERT_TRUE(store.duplicate("LF-245", "LF-245 copy"));
  StoredProfile copy;
  TEST_ASSERT_TRUE(store.load("LF-245 copy", copy));
  TEST_ASSERT_FALSE(copy.stock);                 // a duplicate is always user-owned (§23)
  TEST_ASSERT_EQUAL_UINT(2, copy.phaseCount);    // content preserved
  TEST_ASSERT_TRUE(store.remove("LF-245 copy")); // and now deletable (user)

  // Re-create it, then a second dup onto the same name is refused (no silent clobber).
  TEST_ASSERT_TRUE(store.duplicate("LF-245", "LF-245 copy"));
  TEST_ASSERT_FALSE(store.duplicate("LF-245", "LF-245 copy"));
  // A missing source is refused.
  TEST_ASSERT_FALSE(store.duplicate("ghost", "ghost copy"));
}

// --- Delete removes a user profile ---

void test_remove_user_profile(void) {
  FakeProfileStorage fs;
  ProfileStore store(fs, RecipeMode::Reflow);
  StoredProfile p = mkProfile("Temp", RecipeMode::Reflow, false, 1);
  p.phases[0].uv = false;
  TEST_ASSERT_TRUE(store.save(p));
  TEST_ASSERT_TRUE(store.remove("Temp"));
  StoredProfile out;
  TEST_ASSERT_FALSE(store.load("Temp", out));
  TEST_ASSERT_FALSE(store.remove("Temp")); // already gone
}

// --- §7 never-mixed: a foreign-mode blob in the same storage is invisible to this mode ---

void test_mode_mismatch_ignored(void) {
  FakeProfileStorage fs;
  {
    ProfileStore reflow(fs, RecipeMode::Reflow);
    StoredProfile p = mkProfile("Shared", RecipeMode::Reflow, false, 1);
    p.phases[0].uv = false;
    TEST_ASSERT_TRUE(reflow.save(p));
  }
  // A cure store over the same bytes must not see a reflow profile.
  ProfileStore cure(fs, RecipeMode::Cure);
  Summary rows[ProfileStore::kMaxListed];
  TEST_ASSERT_EQUAL_UINT(0, cure.list(rows, ProfileStore::kMaxListed));
  StoredProfile out;
  TEST_ASSERT_FALSE(cure.load("Shared", out));
}

// --- Corrupt / short / bad-magic blobs are rejected, not mis-parsed ---

void test_corrupt_blobs_rejected(void) {
  FakeProfileStorage fs;
  ProfileStore store(fs, RecipeMode::Reflow);
  StoredProfile p = mkProfile("Good", RecipeMode::Reflow, false, 1);
  p.phases[0].uv = false;
  TEST_ASSERT_TRUE(store.save(p));

  // A short blob under a name.
  fs.put("short", std::vector<uint8_t>(4, 0xFF));
  // A full-length blob with a corrupt magic (first 4 bytes).
  {
    FakeProfileStorage::Entry *e = fs.find("Good");
    std::vector<uint8_t> junk = e->blob;
    junk[0] ^= 0xFF;
    fs.put("badmagic", junk);
  }

  StoredProfile out;
  TEST_ASSERT_FALSE(store.load("short", out));
  TEST_ASSERT_FALSE(store.load("badmagic", out));
  // Only the one valid profile lists.
  Summary rows[ProfileStore::kMaxListed];
  const size_t n = store.list(rows, ProfileStore::kMaxListed);
  TEST_ASSERT_EQUAL_UINT(1, n);
  TEST_ASSERT_EQUAL_STRING("Good", rows[0].name);
}

// --- Name validation guards the filename key (untrusted uploads) ---

void test_name_validation(void) {
  TEST_ASSERT_FALSE(ProfileStore::validName(""));
  TEST_ASSERT_FALSE(ProfileStore::validName("."));
  TEST_ASSERT_FALSE(ProfileStore::validName(".."));
  TEST_ASSERT_FALSE(ProfileStore::validName("a/b"));       // path separator
  TEST_ASSERT_FALSE(ProfileStore::validName("a\\b"));      // backslash
  TEST_ASSERT_FALSE(ProfileStore::validName("bad\tname")); // control byte
  char toolong[kProfileNameCap + 4];
  std::memset(toolong, 'x', sizeof(toolong));
  toolong[sizeof(toolong) - 1] = '\0';
  TEST_ASSERT_FALSE(ProfileStore::validName(toolong));
  TEST_ASSERT_TRUE(ProfileStore::validName("LF-245"));
  TEST_ASSERT_TRUE(ProfileStore::validName("LF-245 copy"));

  // save() honors the same rule.
  FakeProfileStorage fs;
  ProfileStore store(fs, RecipeMode::Reflow);
  StoredProfile p = mkProfile("ok", RecipeMode::Reflow, false, 1);
  p.phases[0].uv = false;
  std::strcpy(p.name, "a/b");
  TEST_ASSERT_FALSE(store.save(p));
  TEST_ASSERT_EQUAL_INT(0, fs.writeCalls);
}

// --- Phase-count bounds: full list persists; empty and over-cap are refused ---

void test_phase_count_bounds(void) {
  FakeProfileStorage fs;
  ProfileStore store(fs, RecipeMode::Reflow);

  // Empty phase list is not a saveable profile.
  StoredProfile empty = mkProfile("Empty", RecipeMode::Reflow, false, 0);
  TEST_ASSERT_FALSE(store.save(empty));

  // Exactly kMaxPhases round-trips.
  StoredProfile full = mkProfile("Full", RecipeMode::Reflow, false, kMaxPhases);
  for (size_t i = 0; i < kMaxPhases; ++i) {
    full.phases[i].uv = false;
  }
  TEST_ASSERT_TRUE(store.save(full));
  StoredProfile out;
  TEST_ASSERT_TRUE(store.load("Full", out));
  TEST_ASSERT_EQUAL_UINT(kMaxPhases, out.phaseCount);

  // A phaseCount past the array bound is refused (belt-and-braces over the fixed array).
  StoredProfile over = mkProfile("Over", RecipeMode::Reflow, false, kMaxPhases);
  over.phaseCount = kMaxPhases + 1;
  TEST_ASSERT_FALSE(store.save(over));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_empty_store);
  RUN_TEST(test_save_load_round_trip);
  RUN_TEST(test_save_stamps_store_mode);
  RUN_TEST(test_list_alphabetical_with_stock);
  RUN_TEST(test_stock_is_read_only);
  RUN_TEST(test_duplicate);
  RUN_TEST(test_remove_user_profile);
  RUN_TEST(test_mode_mismatch_ignored);
  RUN_TEST(test_corrupt_blobs_rejected);
  RUN_TEST(test_name_validation);
  RUN_TEST(test_phase_count_bounds);
  return UNITY_END();
}
