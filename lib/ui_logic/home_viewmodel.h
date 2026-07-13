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

class HomeViewModel {
public:
  // Tile intents — publish which screen a future screen manager should open (C4/C6). Until
  // those screens exist this is observed only by tests.
  void onCurePressed() { lv_subject_set_int(&subj_nav_request, NAV_CURE_SETUP); }
  void onReflowPressed() { lv_subject_set_int(&subj_nav_request, NAV_REFLOW_SETUP); }
  void onProfilesPressed() { lv_subject_set_int(&subj_nav_request, NAV_PROFILES); }
  void onCalibratePressed() { lv_subject_set_int(&subj_nav_request, NAV_CALIBRATE); }
  void onSettingsPressed() { lv_subject_set_int(&subj_nav_request, NAV_SETTINGS); }

  // Machine-state badge: word + colour (RunState).
  static const char *stateText(int run_state);
  static uint32_t stateColor(int run_state);

  // Link indicator: glyph + word + colour (LinkState).
  static const char *linkText(int link_state);
  static uint32_t linkColor(int link_state);

  // The run flow may only start with a healthy link (§9); mode tiles disable otherwise.
  static bool modeEnabled(int link_state);
};

// Process-lifetime singleton used as callback user_data. HomeViewModel is stateless (all state
// lives in the subjects), so one instance safely serves every Home screen instance.
HomeViewModel &home_view_model();
