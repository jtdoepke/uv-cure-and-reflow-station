// profile_draft.h — the editor's in-RAM working profile (Wave R3b of the §2 "CYD is a UI remote"
// split, 2026-07-17). Replaces ProfileStore::StoredProfile now that the store moved to the
// controller: the same fields (name + mode + stock + Phase[]) with no storage dependency. The
// editor mutates a ProfileDraft in place and Save pushes it to the controller (phase_codec ->
// ManagementClient::requestPut). profile_templates.h seeds a fresh one for NEW; an EDIT decodes a
// fetched oven_Profile into one.
//
// Pure C++ over phase.h — no LVGL, no Arduino, no protobuf.
#pragma once

#include <cstddef>
#include <cstring>

#include "IProfileStorage.h" // kProfileNameCap (the profile-name cap, == oven_Profile.name)
#include "phase.h"           // Phase, RecipeMode, kMaxPhases

struct ProfileDraft {
  char name[kProfileNameCap] = {};
  RecipeMode mode = RecipeMode::Reflow;
  bool stock = false;
  size_t phaseCount = 0;
  Phase phases[kMaxPhases] = {};
};

// Name-validation the editor pre-checks before a Save/rename (the controller's
// control::ProfileStore validates authoritatively, §23; these keep the UI from sending an
// obviously-bad name). Moved here from the CYD ProfileStore, which R3b removed.
namespace profile_names {

// A profile name is also the controller's filesystem key (§7): non-empty, fits kProfileNameCap, no
// path separators or control bytes, and not a directory alias.
inline bool valid(const char *name) {
  if (name == nullptr || name[0] == '\0') {
    return false;
  }
  if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0) {
    return false;
  }
  size_t len = 0;
  for (const char *c = name; *c != '\0'; ++c) {
    if (*c == '/' || *c == '\\' || static_cast<unsigned char>(*c) < 0x20) {
      return false;
    }
    if (++len >= kProfileNameCap) {
      return false;
    }
  }
  return true;
}

// A phase name is authoring text, not a filesystem key — lighter: non-empty, no control bytes, fits
// kPhaseNameCap. (No path-separator rule; a phase name never becomes a key.)
inline bool validPhase(const char *name) {
  if (name == nullptr || name[0] == '\0') {
    return false;
  }
  size_t len = 0;
  for (const char *c = name; *c != '\0'; ++c) {
    if (static_cast<unsigned char>(*c) < 0x20) {
      return false;
    }
    if (++len >= kPhaseNameCap) {
      return false;
    }
  }
  return true;
}

} // namespace profile_names
