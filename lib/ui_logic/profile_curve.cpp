#include "profile_curve.h"

#include "panel.h"
#include "theme.h"

using profile_facts::CurvePoint;

namespace {

// Combined data range across both series, so the requested and achievable lines share one set of
// axes (a divergence must read as the lines pulling apart, not as two independently-scaled plots).
struct Range {
  float tMin, tMax, TMin, TMax;
};

Range dataRange(const CurvePoint *a, size_t na, const CurvePoint *b, size_t nb) {
  Range r{0.0f, 0.0f, 0.0f, 0.0f};
  bool any = false;
  auto fold = [&](const CurvePoint *p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
      if (!any) {
        r.tMin = r.tMax = p[i].t;
        r.TMin = r.TMax = p[i].T;
        any = true;
        continue;
      }
      if (p[i].t < r.tMin)
        r.tMin = p[i].t;
      if (p[i].t > r.tMax)
        r.tMax = p[i].t;
      if (p[i].T < r.TMin)
        r.TMin = p[i].T;
      if (p[i].T > r.TMax)
        r.TMax = p[i].T;
    }
  };
  fold(a, na);
  fold(b, nb);
  if (!any) {
    r = Range{0.0f, 1.0f, 0.0f, 1.0f};
  }
  // Degenerate spans (a flat profile, a single point) would divide by zero when scaling — widen to
  // a unit so the line renders as a centred horizontal rule rather than vanishing.
  if (r.tMax - r.tMin < 1e-3f)
    r.tMax = r.tMin + 1.0f;
  if (r.TMax - r.TMin < 1e-3f)
    r.TMax = r.TMin + 1.0f;
  return r;
}

// A line as a child of the plot box, its points scaled from data coords into the plot's pixel box.
// The scaled points are copied into an LVGL-owned buffer freed on delete, so the caller's array
// need not persist. Returns the line object (nullptr if there is nothing to draw).
lv_obj_t *make_line(lv_obj_t *plot, const CurvePoint *pts, size_t n, const Range &r, int32_t w,
                    int32_t h, uint32_t color, bool dashed) {
  if (pts == nullptr || n < 2) {
    return nullptr;
  }
  auto *scaled = static_cast<lv_point_precise_t *>(lv_malloc(n * sizeof(lv_point_precise_t)));
  if (scaled == nullptr) {
    return nullptr;
  }
  const float sx = static_cast<float>(w - 1) / (r.tMax - r.tMin);
  const float sy = static_cast<float>(h - 1) / (r.TMax - r.TMin);
  for (size_t i = 0; i < n; ++i) {
    const float x = (pts[i].t - r.tMin) * sx;
    const float y = static_cast<float>(h - 1) - (pts[i].T - r.TMin) * sy; // invert: hot = top
    scaled[i].x = static_cast<lv_value_precise_t>(x);
    scaled[i].y = static_cast<lv_value_precise_t>(y);
  }

  lv_obj_t *line = lv_line_create(plot);
  lv_obj_remove_style_all(line);
  lv_obj_set_pos(line, 0, 0);
  lv_line_set_points(line, scaled, static_cast<uint32_t>(n)); // stores the pointer, no copy
  lv_obj_set_style_line_color(line, theme::col(color), 0);
  lv_obj_set_style_line_width(line, w > 0 ? (dashed ? theme::HAIRLINE : theme::BRACKET_W) : 1, 0);
  lv_obj_set_style_line_rounded(line, true, 0);
  if (dashed) {
    lv_obj_set_style_line_dash_width(line, theme::GRID_STEP / 3, 0);
    lv_obj_set_style_line_dash_gap(line, theme::GRID_STEP / 3, 0);
  }
  // Own the scaled buffer: free it when the line (and thus the whole curve) is deleted.
  lv_obj_set_user_data(line, scaled);
  lv_obj_add_event_cb(
      line,
      [](lv_event_t *e) {
        lv_free(lv_obj_get_user_data(static_cast<lv_obj_t *>(lv_event_get_target(e))));
      },
      LV_EVENT_DELETE, nullptr);
  return line;
}

} // namespace

ProfileCurve create_profile_curve(lv_obj_t *parent, const CurvePoint *req, size_t n_req,
                                  const CurvePoint *ach, size_t n_ach, bool uncalibrated) {
  ProfileCurve ui{};

  // Root: a themed instrument panel that reads as a chart frame, not an app card.
  ui.root = lv_obj_create(parent);
  theme::apply_panel(ui.root);
  lv_obj_set_width(ui.root, lv_pct(100));
  lv_obj_set_height(ui.root, panel::H / 4); // ~a quarter of the screen; adapts to both panels
  lv_obj_set_style_pad_all(ui.root, theme::PAD_S, 0);
  lv_obj_remove_flag(ui.root, LV_OBJ_FLAG_SCROLLABLE);

  // Plot box: fills the panel, accent-outlined so it reads as an axes frame (the "instrument"
  // line-art). The lines are its children, positioned in its pixel-content coordinates.
  ui.plot = lv_obj_create(ui.root);
  lv_obj_remove_style_all(ui.plot);
  lv_obj_set_size(ui.plot, lv_pct(100), lv_pct(100));
  lv_obj_set_style_pad_all(ui.plot, 0, 0);
  lv_obj_remove_flag(ui.plot, LV_OBJ_FLAG_SCROLLABLE);
  theme::add_hairline(ui.plot);

  // Percentages resolve only once the tree is laid out; force it so the pixel box is real before we
  // scale points into it.
  lv_obj_update_layout(lv_obj_get_screen(ui.root));
  int32_t w = lv_obj_get_content_width(ui.plot);
  int32_t h = lv_obj_get_content_height(ui.plot);
  if (w < 2)
    w = 2;
  if (h < 2)
    h = 2;

  const Range r = dataRange(req, n_req, ach, n_ach);
  // Requested first (under), achievable on top so the real trajectory wins any overlap.
  ui.requested_line = make_line(ui.plot, req, n_req, r, w, h, theme::TEXT_DIM, /*dashed=*/true);
  ui.achievable_line = make_line(ui.plot, ach, n_ach, r, w, h, theme::ACCENT, /*dashed=*/false);

  if (uncalibrated) {
    // §12 "preview is idealized" note, tucked top-left where the curve starts low (t=0, ambient) so
    // it does not collide with the trajectory. Kept to one word — the fonts carry no em dash.
    ui.note = lv_label_create(ui.plot);
    lv_label_set_text(ui.note, "uncalibrated");
    theme::apply_caption(ui.note);
    lv_obj_align(ui.note, LV_ALIGN_TOP_LEFT, theme::PAD_S, theme::PAD_S);
  }
  return ui;
}
