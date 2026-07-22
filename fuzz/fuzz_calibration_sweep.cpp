// Internal-correctness / differential harness: the planned calibration-sweep generator (B9,
// lib/app_logic/calibration_sweep.h).
//
// Like the characterization runs themselves, a sweep profile is one the operator never authored and
// never reviews — the §20 wizard generates it and hands it straight to Start. So it inherits
// fuzz_compiler's core property, raised to the stakes fuzz_remainder describes for the resume
// generator —
//
//   every run the generator emits must compile HARD-VALID and be accepted by the real
//   RecipeValidator, for any scope and any (untrusted, clamped) user cap
//
// — because a sweep run that NAKs on upload strands the calibration mid-sweep, and one that exceeds
// the content-derived hard-max would drive the element past a reviewed ceiling.
//
// The fuzzed inputs are the sweep SCOPE and a user CAP. The cap is clamped DOWN to the controller's
// reviewed reflow hard-max exactly as the real call site does (a user cap can never loosen past it,
// §4/§9); a low/negative/degenerate cap is passed through so the generator's "no band fits →
// refuse" path is exercised too. The same caps go to generateRun and to compileRecipe, and
// RecipeValidator independently enforces the reflow ceiling — so if any run's target slipped above
// the content hard-max, the differential would catch it.
//
// Input format (little-endian):
//   [0]     scope : value % 3 → Quick / Standard / Thorough
//   [1..4]  capC  : float32 user cap (clamped to [MIN_SEGMENT_C, REFLOW_HARD_MAX_C] as above)
#include <cmath>
#include <cstring>

#include "fuzz_util.h"

#include "calibration_sweep.h"
#include "oven_cal.h"
#include "oven_safety.h" // the reviewed reflow hard-max the real call site clamps to
#include "recipe_compiler.h"
#include "recipe_validator.h" // the real controller backstop, as in fuzz_compiler / fuzz_remainder

namespace {

constexpr size_t kHeaderBytes = 5;

float readF32(const uint8_t *p) {
  float f;
  std::memcpy(&f, p, sizeof(f)); // raw bytes → any float, incl. NaN/Inf/denormal
  return f;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size < kHeaderBytes) {
    return 0;
  }

  const cal_sweep::Scope scope = static_cast<cal_sweep::Scope>(data[0] % 3);

  // Clamp the untrusted cap down to the reviewed reflow ceiling: NaN/Inf or any over-cap value maps
  // to the hard-max (the `!(x <= max)` test catches NaN); a low/negative value passes through to
  // exercise the refuse path. minC is the reviewed floor, as the SettingsStore-derived call site.
  float capC = readF32(data + 1);
  if (!(capC <= oven_safety::REFLOW_HARD_MAX_C)) {
    capC = oven_safety::REFLOW_HARD_MAX_C;
  }
  const Caps caps{oven_safety::MIN_SEGMENT_C, capC};

  const cal_sweep::Grid g = cal_sweep::gridFor(scope);
  const OvenModel &model = oven_cal::kDefaultModel;

  // One past the end always refuses.
  ProfileDraft past{};
  FUZZ_ASSERT(!cal_sweep::generateRun(g, cal_sweep::runCount(g), caps, past));

  for (size_t i = 0; i < cal_sweep::runCount(g); ++i) {
    ProfileDraft out{};
    if (!cal_sweep::generateRun(g, i, caps, out)) {
      continue; // refusing (degenerate / too-tight caps) is a valid answer
    }

    // Generator-side invariants: plain-heat REFLOW, within caps, bounded phase count.
    FUZZ_ASSERT(out.mode == RecipeMode::Reflow);
    FUZZ_ASSERT(out.phaseCount >= 1 && out.phaseCount <= kMaxPhases);
    for (size_t k = 0; k < out.phaseCount; ++k) {
      FUZZ_ASSERT(!out.phases[k].uv && !out.phases[k].motor);
      FUZZ_ASSERT(std::isfinite(out.phases[k].targetC));
      FUZZ_ASSERT(out.phases[k].targetC >= caps.minC && out.phases[k].targetC <= caps.capC);
    }

    const CompileResult r = compileRecipe(out.phases, out.phaseCount, RecipeMode::Reflow, model,
                                          caps, cal_sweep::kNominalAmbientC, /*id=*/1, /*seq=*/1);

    // A generated run must ALWAYS compile hard-valid — never a calibration the operator can't run.
    FUZZ_ASSERT(r.hardValid);
    FUZZ_ASSERT(r.recipe.segments_count >= 1);
    FUZZ_ASSERT(r.recipe.segments_count <= kMaxSegments);
    for (pb_size_t s = 0; s < r.recipe.segments_count; ++s) {
      FUZZ_ASSERT(r.recipe.segments[s].dur_ms > 0);
      FUZZ_ASSERT(std::isfinite(r.recipe.segments[s].heat_c));
      // No uv/motor → content-derived REFLOW ceiling; never exceeded.
      FUZZ_ASSERT(r.recipe.segments[s].heat_c <= oven_safety::REFLOW_HARD_MAX_C);
    }

    // THE DIFFERENTIAL: the real controller backstop must accept what the generator produced.
    RecipeValidator validator;
    oven_NakReason reason = oven_NakReason_NAK_UNSPECIFIED;
    FUZZ_ASSERT(validator.validateRecipe(r.recipe, reason));
  }
  return 0;
}
