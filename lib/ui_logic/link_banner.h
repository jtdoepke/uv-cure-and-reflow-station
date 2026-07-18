// A shared "Controller not responding" caution banner (§9/§14). The CYD is a UI remote, so a
// dropped controller link must be visible on every screen, not just Home — every management screen
// drops this in just under its header.
//
// In-flow (a child of the screen's column), NOT a floating top-layer overlay: shown it pushes
// content down; hidden it is excluded from layout and takes no space (LV_OBJ_FLAG_HIDDEN). That is
// what lets one banner sit on any screen without ever covering a top header/Back button or a bottom
// action row — the two places a floating overlay would collide with. Bound to subj_link_state:
// shown for any non-LINK_OK state, hidden when healthy, so it appears in lockstep with the greyed
// link-dependent buttons. Returns the banner (tests assert its hidden flag tracks the subject).
#pragma once

#include <lvgl.h>

lv_obj_t *create_link_banner(lv_obj_t *parent);
