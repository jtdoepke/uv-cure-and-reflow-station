#include "fault_overlay.h"

#include <cstdio>

#include "fault_table.h"
#include "subjects.h"
#include "theme.h"

struct FaultThunks {
  static void ack_evt(lv_event_t *e) {
    static_cast<FaultOverlay *>(lv_event_get_user_data(e))->acknowledge();
  }
};

void FaultOverlay::begin(FaultController &fc) {
  fc_ = &fc;
}

void FaultOverlay::setAckHandler(void (*cb)(void *, AckRoute), void *user_data) {
  on_ack_ = cb;
  ack_ud_ = user_data;
}

void FaultOverlay::poll() {
  if (fc_ == nullptr) {
    return;
  }
  const FaultState s = fc_->state();

  if (!s.active) {
    if (shown_) {
      teardown();
    }
    return;
  }
  // Rebuild only when the latch actually changed — a raise, or a supersede that swapped the cause
  // or bumped the +N count. §22's "don't stack modals" is this: one panel, updated in place.
  if (!shown_ || s.updatedAtMs != shown_at_) {
    build(s);
    shown_at_ = s.updatedAtMs;
    shown_ = true;
    shown_code_ = s.code;
  }
  // The live reading (§22 shows the relevant one under the cause). Refreshed in place, and only on
  // change — this runs every loop.
  if (temp_lbl_ != nullptr) {
    const int t = lv_subject_get_int(&subj_chamber_temp);
    if (t != last_temp_) {
      last_temp_ = t;
      lv_label_set_text_fmt(temp_lbl_, "Chamber  %d\xC2\xB0", t);
    }
  }
}

void FaultOverlay::acknowledge() {
  if (fc_ == nullptr || !shown_) {
    return;
  }
  const AckRoute route = fc_->acknowledge();
  last_route_ = route;
  teardown();
  if (on_ack_ != nullptr) {
    on_ack_(ack_ud_, route);
  }
}

void FaultOverlay::teardown() {
  lv_obj_t *layer = lv_layer_top();
  lv_anim_delete(layer, nullptr); // stop the alarm pulse before its target goes away
  lv_obj_clean(layer);
  // Hand input back to the screen underneath. Leaving this flag set would silently swallow every
  // touch on the app after the first acknowledged fault.
  lv_obj_remove_flag(layer, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_opa(layer, LV_OPA_TRANSP, 0);
  root_ = nullptr;
  temp_lbl_ = nullptr;
  shown_ = false;
  shown_code_ = oven_FaultCode_FAULT_NONE;
  last_temp_ = INT32_MIN;
}

void FaultOverlay::build(const FaultState &s) {
  lv_obj_t *layer = lv_layer_top();
  lv_anim_delete(layer, nullptr);
  lv_obj_clean(layer);
  root_ = nullptr;
  temp_lbl_ = nullptr;
  last_temp_ = INT32_MIN;

  // Modal: absorb every touch that isn't Acknowledge, and dim what's underneath so the screen below
  // reads as suspended rather than merely obscured.
  lv_obj_add_flag(layer, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(layer, theme::col(theme::BG), 0);
  lv_obj_set_style_bg_opa(layer, LV_OPA_70, 0);

  lv_obj_t *panel = lv_obj_create(layer);
  root_ = panel;
  theme::apply_fault_panel(panel);
  lv_obj_set_size(panel, lv_pct(94), lv_pct(94));
  lv_obj_center(panel);
  lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(panel, theme::GAP, 0);

  // Red banner: icon + the word FAULT. Colour NEVER alone (ISA-101) — the glyph and the word carry
  // it, which is also why this line uses the body font: big_font() has no symbol glyphs
  // (fonts/README.md), so a big ⚠ would draw as a missing-glyph box.
  lv_obj_t *banner = lv_obj_create(panel);
  theme::apply_alert(banner, theme::FAULT);
  lv_obj_set_width(banner, lv_pct(100));
  lv_obj_set_height(banner, LV_SIZE_CONTENT);
  lv_obj_t *banner_lbl = lv_label_create(banner);
  lv_label_set_text(banner_lbl, LV_SYMBOL_WARNING "  F A U L T");
  lv_obj_center(banner_lbl);
  // §22's multi-modal annunciation, minus the hardware half: pulse the banner while unacknowledged.
  // The RGB LED + buzzer are still TBD §10 and are NOT wired here.
  theme::alarm_pulse(banner);

  // Everything between the banner and Acknowledge scrolls; those two are PINNED. §22's causes carry
  // real prose — LINK_LOST's two clauses are called out as load-bearing and un-trimmable — and on
  // the 240 px landscape they do not fit under a big-font cause. Letting the text share the slack
  // with flex_grow crushed the guidance to a single row of ellipsis, and letting the whole panel
  // scroll would put Acknowledge below the fold, which is the one control an alarm must always
  // offer. Scrolling the body is the only arrangement where nothing important can be lost.
  lv_obj_t *body = lv_obj_create(panel);
  lv_obj_remove_style_all(body);
  lv_obj_set_width(body, lv_pct(100));
  lv_obj_set_flex_grow(body, 1);
  lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(body, theme::GAP, 0);
  lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(body, LV_DIR_VER);

  // Plain-language cause — the largest text on the panel (§22: "big-text cause first"). formatTitle
  // covers an unrecognized code with the generic wording rather than leaving this blank.
  char title[64];
  fault_table::formatTitle(s.code, title, sizeof(title));
  lv_obj_t *cause = lv_label_create(body);
  lv_label_set_text(cause, title);
  lv_obj_set_width(cause, lv_pct(100));
  lv_label_set_long_mode(cause, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(cause, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(cause, &theme::big_font(), 0);

  // "+N" when more faults latched this episode (§22: update the cause, keep a count, don't stack).
  if (s.count > 1) {
    lv_obj_t *more = lv_label_create(body);
    lv_label_set_text_fmt(more, "+%u more", static_cast<unsigned>(s.count - 1));
    lv_obj_set_width(more, lv_pct(100));
    lv_obj_set_style_text_align(more, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(more, theme::col(theme::FAULT), 0);
  }

  // What the system already did — the reassurance line. This is the guidance string from the table,
  // which for LINK_LOST is deliberately the two-clause §22 wording (it cannot confirm live state,
  // so it reassures via the invariant instead).
  lv_obj_t *guidance = lv_label_create(body);
  lv_label_set_text(guidance, fault_table::guidanceText(s.code));
  lv_obj_set_width(guidance, lv_pct(100));
  lv_label_set_long_mode(guidance, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(guidance, LV_TEXT_ALIGN_CENTER, 0);

  // The relevant live reading.
  temp_lbl_ = lv_label_create(body);
  lv_label_set_text(temp_lbl_, "Chamber  --\xC2\xB0");
  lv_obj_set_width(temp_lbl_, lv_pct(100));
  lv_obj_set_style_text_align(temp_lbl_, LV_TEXT_ALIGN_CENTER, 0);

  // Small, for logs/support. codeName is null for an unrecognized code — print the number instead,
  // which is the thing worth quoting in that case anyway.
  lv_obj_t *code = lv_label_create(body);
  const char *name = fault_table::codeNameText(s.code);
  if (name[0] != '\0') {
    lv_label_set_text_fmt(code, "Code: %s", name);
  } else {
    lv_label_set_text_fmt(code, "Code: %d", static_cast<int>(s.code));
  }
  lv_obj_set_width(code, lv_pct(100));
  lv_obj_set_style_text_align(code, LV_TEXT_ALIGN_CENTER, 0);
  theme::apply_caption(code);

  // Acknowledge — a plain single tap, not press-and-hold: dismissing an alarm is a SAFE action, and
  // the design rules reserve press-and-hold for energizing ones. Full width so it clears the ≥67 px
  // target on both panels; §22's optional Details pane is not built (it wants the raw
  // last-telemetry vector, which belongs with B8·1's log record).
  lv_obj_t *ack = lv_button_create(panel);
  theme::apply_mode_tile(ack);
  lv_obj_set_style_bg_color(ack, theme::col(theme::FAULT), 0);
  lv_obj_set_width(ack, lv_pct(100));
  lv_obj_set_height(ack, theme::SECONDARY_H);
  lv_obj_t *ack_lbl = lv_label_create(ack);
  lv_label_set_text(ack_lbl, "Acknowledge");
  lv_obj_center(ack_lbl);
  lv_obj_add_event_cb(ack, FaultThunks::ack_evt, LV_EVENT_CLICKED, this);
}
