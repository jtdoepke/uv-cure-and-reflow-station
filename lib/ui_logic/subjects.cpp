#include "subjects.h"

#include "settings_defaults.h"

lv_subject_t subj_chamber_temp;
lv_subject_t subj_run_state;
lv_subject_t subj_link_state;
lv_subject_t subj_nav_request;
lv_subject_t subj_units;
lv_subject_t subj_uv_cap;
lv_subject_t subj_reflow_cap;
lv_subject_t subj_advanced;
lv_subject_t subj_has_ambient_light;

void ui_subjects_init() {
  lv_subject_init_int(&subj_chamber_temp, 22);
  lv_subject_init_int(&subj_run_state, RUN_IDLE);
  // Honest boot default: assume no controller until telemetry/handshake proves otherwise, so
  // the run flow stays gated until the link is healthy (§9).
  lv_subject_init_int(&subj_link_state, LINK_NONE);
  lv_subject_init_int(&subj_nav_request, NAV_NONE);
  // Settings mirrors seed to the firmware factory defaults; the Settings screen overwrites them
  // from the loaded SettingsStore at boot.
  lv_subject_init_int(&subj_units, 0);
  lv_subject_init_int(&subj_uv_cap, settings_defaults::UV_CAP_DEFAULT);
  lv_subject_init_int(&subj_reflow_cap, settings_defaults::REFLOW_CAP_DEFAULT);
  lv_subject_init_int(&subj_advanced, 0);
  // Assume fitted; the firmware corrects this from cyd_board.h at boot. Defaulting to "present"
  // keeps the sim and the UI tests on the interesting path — a row that is greyed out by default
  // is a row nobody ever looks at.
  lv_subject_init_int(&subj_has_ambient_light, 1);
}
