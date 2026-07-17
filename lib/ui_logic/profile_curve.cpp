#include "profile_curve.h"

#include <cstdio>

#include "panel.h"
#include "theme.h"

using profile_facts::CurvePoint;
using profile_facts::TimeSpan;

namespace {

// Free the lv_malloc'd point buffer a line owns, when the line (and thus the whole curve) is
// deleted. Shared by the polylines and the phase-separator verticals.
void free_points_cb(lv_event_t *e) {
  lv_free(lv_obj_get_user_data(static_cast<lv_obj_t *>(lv_event_get_target(e))));
}

// A temperature-axis tick ("245°"). Whole units (the chart is indicative, not a readout); guards a
// non-finite value by clamping upstream.
void format_tick_temp(float t, char *buf, size_t n) {
  std::snprintf(buf, n, "%d\xC2\xB0", static_cast<int>(t >= 0.0f ? t + 0.5f : t - 0.5f));
}

// Combined data range across the series, so every trace shares one set of axes (a divergence must
// read as the lines pulling apart, not as independently-scaled plots).
struct Range {
  float tMin, tMax, TMin, TMax;
};

Range dataRange(const CurvePoint *a, size_t na, const CurvePoint *b, size_t nb, const CurvePoint *c,
                size_t nc) {
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
  fold(c, nc);
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

// A polyline as a child of the plot box, its points scaled from data coords into the plot's pixel
// box. The scaled points are copied into an LVGL-owned buffer freed on delete, so the caller's
// array need not persist. Returns the line object (nullptr if there is nothing to draw).
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
  lv_obj_set_user_data(line, scaled);
  lv_obj_add_event_cb(line, free_points_cb, LV_EVENT_DELETE, nullptr);
  return line;
}

// A full-height dashed vertical rule at pixel column `x` — a phase separator.
lv_obj_t *make_vline(lv_obj_t *plot, int32_t x, int32_t h) {
  auto *pts = static_cast<lv_point_precise_t *>(lv_malloc(2 * sizeof(lv_point_precise_t)));
  if (pts == nullptr) {
    return nullptr;
  }
  pts[0].x = static_cast<lv_value_precise_t>(x);
  pts[0].y = 0;
  pts[1].x = static_cast<lv_value_precise_t>(x);
  pts[1].y = static_cast<lv_value_precise_t>(h - 1);

  lv_obj_t *line = lv_line_create(plot);
  lv_obj_remove_style_all(line);
  lv_obj_set_pos(line, 0, 0);
  lv_line_set_points(line, pts, 2);
  lv_obj_set_style_line_color(line, theme::col(theme::GRID), 0);
  lv_obj_set_style_line_width(line, theme::HAIRLINE, 0);
  lv_obj_set_style_line_dash_width(line, theme::GRID_STEP / 4, 0);
  lv_obj_set_style_line_dash_gap(line, theme::GRID_STEP / 4, 0);
  lv_obj_set_user_data(line, pts);
  lv_obj_add_event_cb(line, free_points_cb, LV_EVENT_DELETE, nullptr);
  return line;
}

// A low-opacity violet band spanning [x0, x1] across the full plot height — a UV-on phase (cure).
void make_uv_band(lv_obj_t *plot, int32_t x0, int32_t x1, int32_t h) {
  if (x1 <= x0) {
    x1 = x0 + 1;
  }
  lv_obj_t *band = lv_obj_create(plot);
  lv_obj_remove_style_all(band);
  lv_obj_set_pos(band, x0, 0);
  lv_obj_set_size(band, x1 - x0, h);
  lv_obj_set_style_bg_color(band, theme::col(theme::UV), 0);
  lv_obj_set_style_bg_opa(band, LV_OPA_20, 0);
  lv_obj_remove_flag(band, LV_OBJ_FLAG_SCROLLABLE);
}

// A dim axis-tick label as a child of `parent`, positioned by lv_obj_align. Uses the small font —
// the same one the phase names use, so every chart annotation is one size.
lv_obj_t *make_tick_label(lv_obj_t *parent, const char *text) {
  lv_obj_t *lbl = lv_label_create(parent);
  lv_label_set_text(lbl, text);
  theme::apply_caption(lbl);
  lv_obj_set_style_text_font(lbl, &theme::small_font(), 0);
  return lbl;
}
void add_tick_label(lv_obj_t *parent, const char *text, lv_align_t align, int32_t dx, int32_t dy) {
  lv_obj_align(make_tick_label(parent, text), align, dx, dy);
}

// Data time → pixel column within the plot's content box.
int32_t px_for(float t, const Range &r, float sx) {
  return static_cast<int32_t>((t - r.tMin) * sx);
}

// A label drawn INSIDE the plot, rotated 90° clockwise (reads top→down) and dimmed, its column's
// left edge at pixel column `col_x`, vertically centred in the `plot_h`-tall plot. Rotating about
// the label's CENTRE keeps that centre fixed, so we just position the (unrotated) box so its centre
// lands where the rotated column should be centred — sidestepping the corner-pivot geometry.
void make_vlabel(lv_obj_t *plot, int32_t col_x, int32_t plot_h, const char *text) {
  lv_obj_t *lbl = lv_label_create(plot);
  lv_label_set_text(lbl, text);
  theme::apply_caption(lbl);
  lv_obj_set_style_text_font(lbl, &theme::small_font(), 0);
  lv_obj_set_style_text_opa(lbl, LV_OPA_40, 0); // reduced opacity — an annotation, not a datum
  lv_obj_update_layout(lbl);
  const int32_t lw = lv_obj_get_width(lbl);  // text length → the rotated column's height
  const int32_t lh = lv_obj_get_height(lbl); // line height → the rotated column's width
  lv_obj_set_style_transform_pivot_x(lbl, lw / 2, 0);
  lv_obj_set_style_transform_pivot_y(lbl, lh / 2, 0);
  lv_obj_set_style_transform_rotation(lbl, 900, 0); // 90.0° clockwise
  const int32_t cx = col_x + lh / 2;                // the rotated column's left edge lands at col_x
  const int32_t cy = plot_h / 2;                    // vertically centred in the plot
  lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, cx - lw / 2, cy - lh / 2);
}

// A legend entry: a small colour swatch (a short line-bar, or a translucent square for the UV band)
// followed by a small-font label, laid out as one flex row.
void add_legend_item(lv_obj_t *legend, uint32_t color, bool band, const char *text) {
  lv_obj_t *item = lv_obj_create(legend);
  lv_obj_remove_style_all(item);
  lv_obj_set_size(item, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_gap(item, theme::PAD_S, 0);
  lv_obj_remove_flag(item, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *sw = lv_obj_create(item);
  lv_obj_remove_style_all(sw);
  lv_obj_set_style_bg_color(sw, theme::col(color), 0);
  if (band) {
    lv_obj_set_size(sw, theme::GRID_STEP / 2, theme::GRID_STEP / 2); // a square, like the shading
    lv_obj_set_style_bg_opa(sw, LV_OPA_40, 0);
  } else {
    lv_obj_set_size(sw, theme::GRID_STEP, theme::BRACKET_W); // a short line bar
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
  }

  make_tick_label(item, text);
}

} // namespace

ProfileCurve create_profile_curve(lv_obj_t *parent, const ProfileCurveData &d) {
  ProfileCurve ui{};

  // The legend row below the plot, sized to the small font it uses. The chart draws at most two
  // lines (requested + projected) plus the UV band, so the legend is three short items — one row.
  const bool has_settling = d.overshoot != nullptr && d.n_overshoot >= 2;
  const int32_t line_h = lv_font_get_line_height(&theme::small_font());
  const int32_t legend_h = line_h + theme::PAD_S * 2;

  // Root: a themed instrument panel. A column: plot (grows) then the legend. Phase and time labels
  // live INSIDE the plot (rotated, per phase), so there is no separate axis strip.
  ui.root = lv_obj_create(parent);
  theme::apply_panel(ui.root);
  lv_obj_set_width(ui.root, lv_pct(100));
  lv_obj_set_height(ui.root, panel::H / 4 + legend_h);
  lv_obj_set_style_pad_all(ui.root, theme::PAD_S, 0);
  lv_obj_set_style_pad_row(ui.root, 0, 0);
  lv_obj_set_flex_flow(ui.root, LV_FLEX_FLOW_COLUMN);
  lv_obj_remove_flag(ui.root, LV_OBJ_FLAG_SCROLLABLE);

  // Plot box: the accent-outlined axes frame; the traces + annotations are its children.
  ui.plot = lv_obj_create(ui.root);
  lv_obj_remove_style_all(ui.plot);
  lv_obj_set_width(ui.plot, lv_pct(100));
  lv_obj_set_flex_grow(ui.plot, 1);
  lv_obj_set_style_pad_all(ui.plot, 0, 0);
  lv_obj_remove_flag(ui.plot, LV_OBJ_FLAG_SCROLLABLE);
  theme::add_hairline(ui.plot);

  // Legend row: a centred flex row naming each line/band that is actually drawn.
  ui.legend = lv_obj_create(ui.root);
  lv_obj_remove_style_all(ui.legend);
  lv_obj_set_width(ui.legend, lv_pct(100));
  lv_obj_set_height(ui.legend, legend_h);
  lv_obj_set_flex_flow(ui.legend, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(ui.legend, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(ui.legend, theme::PAD_M, 0);
  lv_obj_set_style_pad_row(ui.legend, 0, 0);
  lv_obj_remove_flag(ui.legend, LV_OBJ_FLAG_SCROLLABLE);
  // Every line/band the chart draws is legended so nothing is an unlabelled mystery: the requested
  // ghost, the settling (predicted actual) trace, and the UV band on cure.
  add_legend_item(ui.legend, d.rate_limited ? theme::WARN : theme::TEXT_DIM, /*band=*/false,
                  "requested");
  if (has_settling) {
    add_legend_item(ui.legend, theme::ACCENT, /*band=*/false, "projected");
  }
  if (d.n_uv_spans > 0) {
    add_legend_item(ui.legend, theme::UV, /*band=*/true, "UV on");
  }

  // Percentages/flex resolve only once laid out; force it so the pixel boxes are real.
  lv_obj_update_layout(lv_obj_get_screen(ui.root));
  int32_t w = lv_obj_get_content_width(ui.plot);
  int32_t h = lv_obj_get_content_height(ui.plot);
  if (w < 2)
    w = 2;
  if (h < 2)
    h = 2;

  // Range over the two drawn series (requested + settling). The rate-limited "achievable" setpoint
  // is not drawn, and settling shares its time extent, so it need not be folded in.
  const Range r = dataRange(d.requested, d.n_requested, d.overshoot, d.n_overshoot, nullptr, 0);
  const float sx = static_cast<float>(w - 1) / (r.tMax - r.tMin);

  // UV-on bands first (furthest back — they are context behind everything).
  for (size_t i = 0; i < d.n_uv_spans; ++i) {
    make_uv_band(ui.plot, px_for(d.uv_spans[i].start, r, sx), px_for(d.uv_spans[i].end, r, sx), h);
    ++ui.uv_bands;
  }

  // Phase-separator rules (grid, under the traces). An internal boundary only — one sitting on an
  // axis edge (t=0 or the final total at the right edge) is not a separator.
  for (size_t i = 0; i < d.n_boundaries; ++i) {
    const float pt = d.boundaries[i];
    if (pt <= r.tMin + 1e-3f || pt >= r.tMax - 1e-3f) {
      continue;
    }
    make_vline(ui.plot, px_for(pt, r, sx), h);
    ++ui.separators;
  }

  // Two lines: the requested trajectory as a dashed ghost (amber when a ramp is optimistic — the
  // §12 divergence flag), and the *settling* trace — the predicted actual temperature — as the
  // solid accent primary. The intermediate rate-limited "achievable" setpoint is not drawn:
  // requested-vs- settling already shows the full gap between what was asked and what the oven will
  // do. Requested first (under), settling on top so the real trajectory wins any overlap.
  // `achievable` is still folded into the axis range so the plot fits it.
  ui.requested_line = make_line(ui.plot, d.requested, d.n_requested, r, w, h,
                                d.rate_limited ? theme::WARN : theme::TEXT_DIM, /*dashed=*/true);
  ui.overshoot_line =
      make_line(ui.plot, d.overshoot, d.n_overshoot, r, w, h, theme::ACCENT, /*dashed=*/false);

  // Y axis: temperature extremes on the left (peak top, min bottom).
  char tbuf[12];
  format_tick_temp(r.TMax, tbuf, sizeof(tbuf));
  add_tick_label(ui.plot, tbuf, LV_ALIGN_TOP_LEFT, 1, 1);
  format_tick_temp(r.TMin, tbuf, sizeof(tbuf));
  add_tick_label(ui.plot, tbuf, LV_ALIGN_BOTTOM_LEFT, 1, -1);

  // Phase labels INSIDE the plot: one per phase, "<name> <start time>s", rotated 90° clockwise and
  // dimmed, placed just right of the phase's start line — the y-axis for the first phase, each
  // internal boundary rule for the rest. The start time is the phase's own start (0, then each
  // boundary), so the whole time axis is read off these rather than a separate strip.
  const size_t names = d.n_phase_names < d.n_boundaries ? d.n_phase_names : d.n_boundaries;
  for (size_t i = 0; i < names; ++i) {
    if (d.phase_names[i] == nullptr) {
      continue;
    }
    const float startT = i == 0 ? r.tMin : d.boundaries[i - 1];
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%s %ds", d.phase_names[i], static_cast<int>(startT + 0.5f));
    make_vlabel(ui.plot, px_for(startT, r, sx) + theme::HAIRLINE + 1, h, buf);
  }

  // The run's end time, as a vertical label tucked just inside the right edge — where the projected
  // trace finishes (the real end-to-end duration, the same figure as the facts line).
  {
    char endbuf[16];
    std::snprintf(endbuf, sizeof(endbuf), "%ds", static_cast<int>(r.tMax + 0.5f));
    make_vlabel(ui.plot, w - line_h - theme::HAIRLINE, h, endbuf);
  }

  if (d.uncalibrated) {
    // §12 "preview is idealized" note, top-right (the top-left holds the peak-temp tick). One word
    // — the fonts carry no em dash.
    ui.note = lv_label_create(ui.plot);
    lv_label_set_text(ui.note, "uncalibrated");
    theme::apply_caption(ui.note);
    lv_obj_align(ui.note, LV_ALIGN_TOP_RIGHT, -theme::PAD_S, theme::PAD_S);
  }

  return ui;
}
