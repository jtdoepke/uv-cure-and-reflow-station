#include "link_banner.h"

#include "subjects.h"
#include "theme.h"

lv_obj_t *create_link_banner(lv_obj_t *parent) {
  // The shared caution treatment (amber wash + edge + text) so this reads as the same kind of
  // object as every other abnormal-state banner (§13/§22), not as loose amber text.
  lv_obj_t *banner = lv_obj_create(parent);
  theme::apply_alert(banner, theme::WARN);
  lv_obj_set_width(banner, lv_pct(100));
  lv_obj_set_height(banner, theme::BANNER_H);

  lv_obj_t *text = lv_label_create(banner);
  lv_label_set_text(text, LV_SYMBOL_WARNING " Controller not responding");
  lv_obj_center(text);

  // Hidden (and out of layout) while the link is healthy; shown for any other state. Same predicate
  // the tiles gate on, so the banner and the greyed buttons appear and clear together.
  lv_obj_bind_flag_if_eq(banner, &subj_link_state, LV_OBJ_FLAG_HIDDEN, LINK_OK);
  return banner;
}
