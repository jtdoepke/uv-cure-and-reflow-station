#include "selectable_list.h"

#include <cstdint>

#include "theme.h"

// --- Model ---

void SelectableListModel::init(const SelectableListItem *items, int count, bool wrap) {
  count_ = count < 0 ? 0 : (count > kMaxItems ? kMaxItems : count);
  for (int i = 0; i < count_; i++) {
    items_[i] = items[i];
  }
  wrap_ = wrap;
  on_open_ = nullptr;
  open_ud_ = nullptr;
  int first = firstEnabled();
  lv_subject_init_int(&selected_subject_, first < 0 ? 0 : first);
}

int SelectableListModel::firstEnabled() const {
  for (int i = 0; i < count_; i++) {
    if (items_[i].enabled) {
      return i;
    }
  }
  return -1;
}

int SelectableListModel::lastEnabled() const {
  for (int i = count_ - 1; i >= 0; i--) {
    if (items_[i].enabled) {
      return i;
    }
  }
  return -1;
}

int SelectableListModel::nextEnabled(int from) const {
  for (int i = from + 1; i < count_; i++) {
    if (items_[i].enabled) {
      return i;
    }
  }
  return from;
}

int SelectableListModel::prevEnabled(int from) const {
  for (int i = from - 1; i >= 0; i--) {
    if (items_[i].enabled) {
      return i;
    }
  }
  return from;
}

void SelectableListModel::moveUp() {
  int prev = prevEnabled(selected());
  if (wrap_ && prev == selected()) { // already at the first enabled row → wrap to the last
    int last = lastEnabled();
    if (last >= 0) {
      prev = last;
    }
  }
  lv_subject_set_int(&selected_subject_, prev);
}

void SelectableListModel::moveDown() {
  int next = nextEnabled(selected());
  if (wrap_ && next == selected()) { // already at the last enabled row → wrap to the first
    int first = firstEnabled();
    if (first >= 0) {
      next = first;
    }
  }
  lv_subject_set_int(&selected_subject_, next);
}

void SelectableListModel::select(int index) {
  if (index >= 0 && index < count_ && items_[index].enabled) {
    lv_subject_set_int(&selected_subject_, index);
  }
}

void SelectableListModel::onOpen() {
  int sel = selected();
  if (on_open_ != nullptr && sel >= 0 && sel < count_ && items_[sel].enabled) {
    on_open_(sel, open_ud_);
  }
}

void SelectableListModel::setOpenHandler(void (*cb)(int, void *), void *user_data) {
  on_open_ = cb;
  open_ud_ = user_data;
}

bool SelectableListModel::atFirstEnabled() {
  int first = firstEnabled();
  // Under wrap, Up always has somewhere to go unless there's ≤1 selectable row (first == last).
  if (wrap_) {
    return first < 0 || first == lastEnabled();
  }
  return first < 0 || selected() <= first;
}

bool SelectableListModel::atLastEnabled() {
  int last = lastEnabled();
  if (wrap_) {
    return last < 0 || last == firstEnabled();
  }
  return last < 0 || selected() >= last;
}

// --- View ---

namespace {

// Highlight follows the selection: the selected row gets the SELECTED fill and scrolls into view;
// every other row falls back to the plain surface. One observer per row; the row carries its own
// index in its user_data, the model comes through the observer user_data.
void on_selection_changed(lv_observer_t *observer, lv_subject_t *subject) {
  lv_obj_t *row = lv_observer_get_target_obj(observer);
  int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(row)));
  bool selected = idx == lv_subject_get_int(subject);
  lv_obj_set_style_bg_color(row, theme::col(selected ? theme::SELECTED : theme::SURFACE), 0);
  if (selected) {
    lv_obj_scroll_to_view(row, LV_ANIM_OFF);
  }
}

void on_up_state(lv_observer_t *observer, lv_subject_t *) {
  lv_obj_t *btn = lv_observer_get_target_obj(observer);
  auto *m = static_cast<SelectableListModel *>(lv_observer_get_user_data(observer));
  lv_obj_set_state(btn, LV_STATE_DISABLED, m->atFirstEnabled());
}

void on_down_state(lv_observer_t *observer, lv_subject_t *) {
  lv_obj_t *btn = lv_observer_get_target_obj(observer);
  auto *m = static_cast<SelectableListModel *>(lv_observer_get_user_data(observer));
  lv_obj_set_state(btn, LV_STATE_DISABLED, m->atLastEnabled());
}

// The action button names what Open does to the highlighted row (its `verb`, or "Open").
void on_action_verb_changed(lv_observer_t *observer, lv_subject_t *subject) {
  lv_obj_t *label = lv_observer_get_target_obj(observer);
  auto *m = static_cast<SelectableListModel *>(lv_observer_get_user_data(observer));
  int sel = lv_subject_get_int(subject);
  const char *verb = (sel >= 0 && sel < m->count()) ? m->item(sel).verb : nullptr;
  lv_label_set_text(label, verb != nullptr ? verb : "Open");
}

void on_row_clicked(lv_event_t *e) {
  auto *m = static_cast<SelectableListModel *>(lv_event_get_user_data(e));
  auto *row = static_cast<lv_obj_t *>(lv_event_get_target(e));
  m->select(static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(row))));
}

// Footer-button thunk: recover the model and invoke one intent.
#define LIST_INTENT(method)                                                                        \
  [](lv_event_t *e) { static_cast<SelectableListModel *>(lv_event_get_user_data(e))->method(); }

lv_obj_t *make_footer_button(lv_obj_t *parent, const char *text, SelectableListModel &model,
                             lv_event_cb_t on_click) {
  lv_obj_t *btn = lv_button_create(parent);
  theme::apply_secondary(btn);
  lv_obj_set_flex_grow(btn, 1);
  lv_obj_set_height(btn, lv_pct(100));
  lv_obj_t *label = lv_label_create(btn);
  lv_label_set_text(label, text);
  lv_obj_center(label);
  lv_obj_add_event_cb(btn, on_click, LV_EVENT_CLICKED, &model);
  return btn;
}

} // namespace

SelectableList create_selectable_list(lv_obj_t *parent, SelectableListModel &model) {
  SelectableList ui{};
  ui.root = parent;
  lv_subject_t *sel = model.selectedSubject();

  theme::apply_screen(parent);
  lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(parent, theme::PAD_S, 0);
  lv_obj_set_style_pad_row(parent, theme::GAP, 0);

  // --- Scrolling row container: grows to fill the space above the footer ---
  ui.list = lv_obj_create(parent);
  theme::apply_row(ui.list);
  lv_obj_set_width(ui.list, lv_pct(100));
  lv_obj_set_flex_grow(ui.list, 1);
  lv_obj_set_flex_flow(ui.list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(ui.list, theme::PAD_S, 0);
  lv_obj_add_flag(ui.list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(ui.list, LV_DIR_VER);

  for (int i = 0; i < model.count(); i++) {
    const SelectableListItem &it = model.item(i);
    lv_obj_t *row = lv_obj_create(ui.list);
    theme::apply_list_row(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, theme::LIST_ROW_H);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_user_data(row, reinterpret_cast<void *>(static_cast<intptr_t>(i)));

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, it.label);
    if (it.value != nullptr) {
      lv_obj_t *value = lv_label_create(row);
      lv_label_set_text(value, it.value);
      lv_obj_set_style_text_color(value, theme::col(theme::TEXT_DIM), 0);
    }

    if (it.enabled) {
      lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(row, on_row_clicked, LV_EVENT_CLICKED, &model);
      lv_subject_add_observer_obj(sel, on_selection_changed, row, nullptr);
    } else {
      // "Coming soon": greyed, not selectable, not part of the highlight cycle.
      lv_obj_set_style_text_color(row, theme::col(theme::TEXT_DIM), 0);
      lv_obj_set_style_bg_color(row, theme::col(theme::TILE_DISABLED), 0);
    }
    ui.rows[i] = row;
  }

  // --- Footer: Up / Down / Open (big buttons — the real touch targets) ---
  lv_obj_t *footer = lv_obj_create(parent);
  theme::apply_row(footer);
  lv_obj_set_width(footer, lv_pct(100));
  lv_obj_set_height(footer, theme::SECONDARY_H);
  lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_ROW);

  // Up/Down show Font Awesome chevrons (LV_SYMBOL_UP/DOWN, 0xF077/0xF078) — carried by the 14 px
  // default font (see fonts/README.md). Open keeps a word since no matching glyph is embedded.
  // Up/Down show Font Awesome chevrons (LV_SYMBOL_UP/DOWN, 0xF077/0xF078) — carried by the 14 px
  // default font (see fonts/README.md).
  ui.btn_up = make_footer_button(footer, LV_SYMBOL_UP, model, LIST_INTENT(moveUp));
  ui.btn_down = make_footer_button(footer, LV_SYMBOL_DOWN, model, LIST_INTENT(moveDown));
  lv_subject_add_observer_obj(sel, on_up_state, ui.btn_up, &model);
  lv_subject_add_observer_obj(sel, on_down_state, ui.btn_down, &model);

  // The action button's label tracks the highlighted row's verb (default "Open").
  ui.btn_open = lv_button_create(footer);
  theme::apply_secondary(ui.btn_open);
  lv_obj_set_flex_grow(ui.btn_open, 1);
  lv_obj_set_height(ui.btn_open, lv_pct(100));
  lv_obj_t *open_label = lv_label_create(ui.btn_open);
  lv_obj_center(open_label);
  lv_obj_add_event_cb(ui.btn_open, LIST_INTENT(onOpen), LV_EVENT_CLICKED, &model);
  lv_subject_add_observer_obj(sel, on_action_verb_changed, open_label, &model);

  return ui;
}
