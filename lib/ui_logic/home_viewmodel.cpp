#include "home_viewmodel.h"

#include "theme.h"

const char *HomeViewModel::badgeText(int run_state, int link_state) {
  // Link trouble is shown on the badge itself now (no separate link readout). A distinct word for
  // each so the operator knows which fix it is: reseat the cable vs reflash the matched pair (§9).
  if (link_state == LINK_NONE) {
    return "NO LINK";
  }
  if (link_state == LINK_SCHEMA) {
    return "SCHEMA";
  }
  switch (run_state) {
  case RUN_FAULT:
    return "FAULT";
  case RUN_RUNNING:
    // Fail-safe amber even though a run normally never shows on Home (§14).
    return "RUNNING";
  case RUN_HOT:
    // Idle AND hot only: runStateFrom never returns RUN_HOT mid-run, so "HOT" can't show cold.
    return "HOT";
  default:
    return "IDLE";
  }
}

uint32_t HomeViewModel::badgeColor(int run_state, int link_state) {
  if (link_state != LINK_OK) {
    return theme::FAULT; // red — controller unreachable or schema-skewed (§9)
  }
  if (run_state == RUN_FAULT) {
    return theme::FAULT; // red — controller fault
  }
  if (run_state == RUN_HOT || run_state == RUN_RUNNING) {
    return theme::WARN; // amber — a run in progress, or a hot (not touch-safe) idle chamber
  }
  return theme::IDLE; // green — idle, cool, and linked
}

uint32_t HomeViewModel::chamberColor(int celsius) {
  if (celsius >= kHotC) {
    return theme::FAULT; // red — a genuine burn risk
  }
  if (celsius >= kTouchSafeC) {
    return theme::WARN; // amber — warm, past the touch-safe point
  }
  return theme::TEXT; // white/primary — touch-safe
}

int HomeViewModel::runStateFrom(bool running, bool faulted, bool hot) {
  // Severity order: a fault is the safety-critical state and must never be masked by "running" or
  // "hot" (§14). A live run is next. Otherwise the machine is idle — but idle-and-hot gets the HOT
  // treatment so the operator is warned the chamber is still dangerous to touch (§14/§17), which is
  // also what keeps sleep suppressed while it cools (main.cpp only sleeps when RUN_IDLE).
  if (faulted) {
    return RUN_FAULT;
  }
  if (running) {
    return RUN_RUNNING;
  }
  return hot ? RUN_HOT : RUN_IDLE;
}

int HomeViewModel::linkStateFrom(bool saw_peer, bool matched, bool alive) {
  // Liveness first, because it is the only input that decays. No telemetry arriving means
  // nothing is out there *right now*, whatever we once heard — and `saw_peer`/`matched` both
  // latch, so consulting them first would keep claiming a link over an unplugged cable.
  // `!saw_peer` with telemetry flowing is the brief pre-handshake window at boot: real, but not
  // something we can call healthy yet, so it reads as no-link too (fail-closed).
  if (!alive || !saw_peer) {
    return LINK_NONE;
  }
  // Something is there and talking; the only question left is whether we agree on the .proto.
  // A peer we cannot trust is not a healthy link — the mode tiles stay disabled either way (§9),
  // but the operator needs to know which of "check the cable" and "reflash the pair" to do.
  return matched ? LINK_OK : LINK_SCHEMA;
}

bool HomeViewModel::modeEnabled(int link_state) {
  return link_state == LINK_OK;
}

HomeViewModel &home_view_model() {
  static HomeViewModel vm;
  return vm;
}
