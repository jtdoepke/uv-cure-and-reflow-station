#include "subjects.h"

lv_subject_t subj_chamber_temp;
lv_subject_t subj_run_state;
lv_subject_t subj_link_state;
lv_subject_t subj_nav_request;

void ui_subjects_init() {
  lv_subject_init_int(&subj_chamber_temp, 22);
  lv_subject_init_int(&subj_run_state, RUN_IDLE);
  // Honest boot default: assume no controller until telemetry/handshake proves otherwise, so
  // the run flow stays gated until the link is healthy (§9).
  lv_subject_init_int(&subj_link_state, LINK_NONE);
  lv_subject_init_int(&subj_nav_request, NAV_NONE);
}
