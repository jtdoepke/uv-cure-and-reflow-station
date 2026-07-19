// HomeViewModel — the thin logic layer between the Home view (§14) and the shared subjects.
// Intent methods publish a navigation request; the static mappers turn machine/link state into
// the text, colour, and enablement the view renders. Pure logic (no widget calls), so it is
// unit-tested directly in native_ui_cyd alongside the rendered-behaviour tests.
//
// Every state is expressed as word/glyph + colour, never colour alone — the colour-blind-safety
// rule (§13/§14): the mappers always pair a colour with text.
#pragma once

#include <lvgl.h>

#include "subjects.h"
#include "touch_safe.h" // the ONE codebase-wide touch-safe temperature (oven_domain::kTouchSafeC)

class HomeViewModel {
public:
  // Tile intents — publish which screen a future screen manager should open (C4/C6). Until
  // those screens exist this is observed only by tests.
  void onCurePressed() { lv_subject_set_int(&subj_nav_request, NAV_CURE_SETUP); }
  void onReflowPressed() { lv_subject_set_int(&subj_nav_request, NAV_REFLOW_SETUP); }
  void onProfilesPressed() { lv_subject_set_int(&subj_nav_request, NAV_PROFILES); }
  void onCalibratePressed() { lv_subject_set_int(&subj_nav_request, NAV_CALIBRATE); }
  void onSettingsPressed() { lv_subject_set_int(&subj_nav_request, NAV_SETTINGS); }

  // The single Home status badge: word + dot colour, folding machine state AND link health into one
  // indicator (§14, revised). It is the only status readout on Home now — no separate link glyph.
  // Priority: a bad link (absent or schema-skewed) or a controller fault is RED; a run in progress
  // is AMBER "RUNNING"; a hot idle chamber (at/above touch-safe) is AMBER "HOT"; a cool idle
  // chamber with a healthy link is GREEN "IDLE". RUNNING and HOT are DISTINCT words on purpose:
  // runStateFrom only returns RUN_HOT when idle AND hot, so "HOT" can never appear over a cold
  // chamber mid-run (the bug this fixes — a run starts cold, and folding RUNNING into "HOT" showed
  // "HOT" at 34 °C). We keep an amber RUNNING badge as a fail-safe even though a run normally
  // leaves Home for the Run screen (§14): if we are somehow on Home during a run, the badge must
  // never read a reassuring green IDLE. Colour is always paired with a word (never colour alone —
  // §13/§14 colour-blind rule).
  static const char *badgeText(int run_state, int link_state);
  static uint32_t badgeColor(int run_state, int link_state);

  // The live chamber readout's text colour, driven by the temperature itself (§14/§17): the
  // number stays legible white while the chamber is touch-safe, warms to AMBER as it climbs, and
  // goes RED once it is genuinely dangerous — so the colour reinforces the digits, never replaces
  // them. Thresholds are in °C (the stored unit), independent of the °F display toggle: below the
  // implicit-cool-phase touch-safe point (kTouchSafeC) is white, below kHotC is amber, at/above is
  // red. Returns a theme colour id (see theme::col), matching the other mappers' convention.
  // kTouchSafeC is the single shared oven_domain::kTouchSafeC (touch_safe.h), as an int since the
  // readout compares whole °C; kHotC is a CYD-readout-only burn line, not a controller constant.
  static constexpr int kTouchSafeC = static_cast<int>(oven_domain::kTouchSafeC);
  static constexpr int kHotC = 60; // above here the chamber is a genuine burn risk
  static uint32_t chamberColor(int celsius);

  // Map the controller's telemetry into the Home badge's RunState (§14). Takes plain bools rather
  // than protocol/nanopb types so ui_logic keeps no dependency on the wire layer; the firmware
  // derives them from a fresh Telemetry frame: `faulted` = a fault code is set or run_state is
  // FAULT; `running` = run_state is RUNNING; `hot` = the chamber is above the touch-safe threshold
  // (the firmware owns that threshold). Fault wins over running wins over hot; otherwise idle.
  // Returns int (not RunState) to match the subject's type and the other mappers' signatures.
  static int runStateFrom(bool running, bool faulted, bool hot);

  // The machine is "at rest" — idle AND touch-safe AND not faulted — which is exactly the RUN_IDLE
  // state the green status dot shows (badgeColor returns IDLE only here). This is the ONE predicate
  // both the idle dot and the §17 screen-sleep gate key off, so "when is the dot green" and "when
  // may the screen sleep" cannot drift apart: the screen may sleep ONLY at rest. Its negation is
  // "a run is in progress, or the chamber is above touch-safe, or a fault is latched" — every case
  // in which a dark screen would hide something the operator must see (§17/§22), and during a run a
  // dark screen would also stall the heartbeat and abort the controller to safe (§9). Deliberately
  // link-independent (unlike the dot's colour, which also reddens on a bad link): a cool, idle
  // machine may sleep even with the controller unplugged — the banner explains the link on wake.
  static bool atRest(int run_state) { return run_state == RUN_IDLE; }

  // The §9 link, as a LinkState. Takes plain bools rather than protocol types so ui_logic keeps
  // no dependency on the protocol layer or nanopb; the firmware feeds it
  // handshake().sawPeer() / .matched() / linkAlive(). Returns int (not LinkState) to match the
  // subject's type and the other mappers' signatures.
  //
  // `alive` (telemetry still arriving) is what makes this decay — `matched` latches, so it would
  // happily report a healthy link over a cable that came out ten minutes ago. `matched` still
  // has to be consulted separately, because a schema-skewed peer is a different problem from an
  // absent one and only one of them is fixed by plugging a cable back in.
  static int linkStateFrom(bool saw_peer, bool matched, bool alive);

  // The run flow may only start with a healthy link (§9); mode tiles disable otherwise.
  static bool modeEnabled(int link_state);
};

// Process-lifetime singleton used as callback user_data. HomeViewModel is stateless (all state
// lives in the subjects), so one instance safely serves every Home screen instance.
HomeViewModel &home_view_model();
