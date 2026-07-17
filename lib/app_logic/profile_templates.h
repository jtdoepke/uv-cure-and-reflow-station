// profile_templates.h — the fixed per-mode phase templates the editor seeds a NEW profile from, and
// the human role labels the editor shows for the fixed phases (design.md §12; backlog C5).
//
// §12's "Structure (DECIDED)": the default editor edits a fixed template per mode — reflow
// preheat/soak/reflow, cure warm/cure — you edit each phase's parameters, not the structure. There
// is no authored cool phase: every run ends with an implicit passive cool-down to a touch-safe temp
// that the compiler/preview append automatically (implicit_cool.h, §6). Create-from-scratch = a new
// profile seeded from that template. (The Advanced path may then add/remove/reorder phases, at
// which point the role labels fall back to generic "Phase N".)
//
// A Phase now carries an explicit stored name (phase.h); this file owns the *seed* for it — the
// role a phase plays at its position in the canonical template. The name is seeded here at creation
// (defaultTemplate + seedPhaseName) and thereafter is plain editable text; there is no runtime
// positional derivation. phaseLabel() survives only as the seed-string source (and the "Phase N"
// fallback for an off-template Advanced add). Kept pure (no LVGL/Arduino) so the templates can be
// asserted compileRecipe()-clean under native_logic_cyd. The seeded values are conservative
// defaults; the operator edits them, and validation is the compiler's (recipe_compiler.h).
#pragma once

#include <cstddef>
#include <cstdio>
#include <cstring>

#include "phase.h"
#include "profile_store.h"

namespace profile_templates {

// The canonical phase counts of the fixed templates. A profile with exactly this many phases is
// shown with role labels; anything else (an Advanced add/remove) falls back to "Phase N".
inline constexpr size_t kReflowPhases = 3;
inline constexpr size_t kCurePhases = 2;

// Role labels for the fixed templates, indexed by phase position. Borrowed literals (never freed).
// No "Cool" role — the cool-down is implicit and appended by the preview/compiler
// (implicit_cool.h).
inline constexpr const char *kReflowRoles[kReflowPhases] = {"Preheat", "Soak", "Reflow"};
inline constexpr const char *kCureRoles[kCurePhases] = {"Warm", "Cure"};

// The label the editor shows for phase `index` of a `count`-phase profile in `mode`. When the count
// matches the canonical template the fixed roles apply; otherwise (Advanced edited the structure)
// the phase is generic. Returns a borrowed literal for the fixed case and, for the generic case, a
// formatted "Phase N" written into `buf` (caller-owned; keeps the return borrowable either way).
inline const char *phaseLabel(RecipeMode mode, size_t index, size_t count, char *buf, size_t n) {
  if (mode == RecipeMode::Reflow && count == kReflowPhases && index < kReflowPhases) {
    return kReflowRoles[index];
  }
  if (mode == RecipeMode::Cure && count == kCurePhases && index < kCurePhases) {
    return kCureRoles[index];
  }
  std::snprintf(buf, n, "Phase %zu", index + 1);
  return buf;
}

// Seed `p.name` from the phase's role at position `index` of a `count`-phase `mode` profile (the
// fixed role, or "Phase N" off-template). Used at template creation and Advanced "+ Add" so a phase
// is never nameless — phases carry an explicit stored name (phase.h); after seeding it is just
// editable text.
inline void seedPhaseName(RecipeMode mode, size_t index, size_t count, Phase &p) {
  char buf[kPhaseNameCap];
  const char *role = phaseLabel(mode, index, count, buf, sizeof(buf));
  std::strncpy(p.name, role, kPhaseNameCap - 1);
  p.name[kPhaseNameCap - 1] = '\0';
}

// A single generic phase — what Advanced "+ Add" appends, and the fallback when a template is
// empty. A gentle warm-up so it is immediately valid: a short ramp to a modest hold, fans on Auto.
inline Phase blankPhase() {
  Phase p;
  p.targetC = 60.0f;
  p.rampSeconds = 60.0f;
  p.holdSeconds = 30.0f;
  return p;
}

// The default template for a mode — the seed for a fresh profile (§12). Name is left empty (the
// editor's name-entry supplies it on the first Save); stock is false (a user profile from the
// start). Targets sit within the factory caps (reflow ≤250, cure ≤100) so a fresh template is
// hard-valid on the default settings; the operator edits from here.
inline ProfileStore::StoredProfile defaultTemplate(RecipeMode mode) {
  ProfileStore::StoredProfile t;
  t.name[0] = '\0';
  t.mode = mode;
  t.stock = false;

  if (mode == RecipeMode::Reflow) {
    t.phaseCount = kReflowPhases;
    // Preheat: ramp up to 150 °C.
    t.phases[0].targetC = 150.0f;
    t.phases[0].rampSeconds = 90.0f;
    t.phases[0].holdSeconds = 0.0f;
    // Soak: gentle rise to 180 °C, hold to equalise.
    t.phases[1].targetC = 180.0f;
    t.phases[1].rampSeconds = 90.0f;
    t.phases[1].holdSeconds = 90.0f;
    // Reflow: fast to peak, short hold above liquidus. The implicit cool-down (implicit_cool.h)
    // takes it back to a touch-safe temperature from here — no authored cool phase.
    t.phases[2].targetC = 245.0f;
    t.phases[2].rampSeconds = 40.0f;
    t.phases[2].holdSeconds = 30.0f;
  } else {
    t.phaseCount = kCurePhases;
    // Warm: bring the chamber up to the cure temperature.
    t.phases[0].targetC = 50.0f;
    t.phases[0].rampSeconds = 60.0f;
    t.phases[0].holdSeconds = 0.0f;
    // Cure: UV on with the turntable, dose authored as exposure-per-surface; holdSeconds is the
    // raw-seconds fallback the compiler uses until beamCoverage is calibrated (§5/§12).
    t.phases[1].targetC = 60.0f;
    t.phases[1].rampSeconds = 30.0f;
    t.phases[1].holdSeconds = 120.0f;
    t.phases[1].exposurePerSurface = 30.0f;
    t.phases[1].uv = true;
    t.phases[1].motor = true;
    // The implicit cool-down (implicit_cool.h) brings the chamber back to a touch-safe temperature
    // from here — no authored cool phase.
  }
  // Stamp each phase's name from its canonical role (Preheat/Soak/Reflow, Warm/Cure).
  for (size_t i = 0; i < t.phaseCount; ++i) {
    seedPhaseName(mode, i, t.phaseCount, t.phases[i]);
  }
  return t;
}

} // namespace profile_templates
