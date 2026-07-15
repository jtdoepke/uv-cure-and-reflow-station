#include "home_viewmodel.h"

#include "theme.h"

const char *HomeViewModel::stateText(int run_state) {
  switch (run_state) {
  case RUN_IDLE:
    return "IDLE";
  case RUN_HOT:
    return "HOT";
  case RUN_RUNNING:
    return "RUNNING";
  case RUN_FAULT:
    return "FAULT";
  default:
    return "--";
  }
}

uint32_t HomeViewModel::stateColor(int run_state) {
  switch (run_state) {
  case RUN_IDLE:
    return theme::IDLE; // green — safe / cool
  case RUN_HOT:
    return theme::WARN; // amber — hot surface
  case RUN_RUNNING:
    return theme::WARN; // amber — energised (Home is idle-only, but keep it honest)
  case RUN_FAULT:
    return theme::FAULT; // red — danger
  default:
    return theme::TEXT_DIM;
  }
}

const char *HomeViewModel::linkText(int link_state) {
  switch (link_state) {
  case LINK_OK:
    return LV_SYMBOL_OK " Link";
  case LINK_SCHEMA:
    return LV_SYMBOL_WARNING " Schema";
  case LINK_NONE:
  default:
    return LV_SYMBOL_CLOSE " No link";
  }
}

uint32_t HomeViewModel::linkColor(int link_state) {
  return link_state == LINK_OK ? theme::IDLE : theme::FAULT;
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
