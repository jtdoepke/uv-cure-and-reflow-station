#include "profile_library_screen.h"

#include <initializer_list>
#include <new> // std::nothrow — the prefetch cache degrades instead of throwing

#include "confirm_dialog.h"
#include "link_banner.h"   // shared "Controller not responding" banner (§9/§14)
#include "name_keyboard.h" // the shared name-entry keyboard (profile Rename)
#include "profile_curve.h"
#include "subjects.h"
#include "theme.h"

// One WINDOW of the library binds to one SelectableListModel (§23 paging — ▲/▼ walks the window
// and asks for the next at its edges), so the model must hold a whole window. It no longer has to
// hold the whole library: that grew to ProfileStore::kMaxListed and never reaches the CYD at once.
// (The header keeps this dependency out of the generic widget; the assert lives here where both
// types are known.)
static_assert(ProfileLibraryViewModel::kMaxRows <=
                  static_cast<size_t>(SelectableListModel::kMaxItems),
              "SelectableListModel::kMaxItems must cover a full profile-library window");

// Captureless thunks — a friend of ProfileLibraryScreen so they can reach its private navigation /
// action methods (the codebase's single-void*-user_data idiom, no std::function).
struct ProfileThunks {
  static void choose_cure(lv_event_t *e) {
    static_cast<ProfileLibraryScreen *>(lv_event_get_user_data(e))->openMode(RecipeMode::Cure);
  }
  static void choose_reflow(lv_event_t *e) {
    static_cast<ProfileLibraryScreen *>(lv_event_get_user_data(e))->openMode(RecipeMode::Reflow);
  }
  static void list_open(int index, void *ud) {
    static_cast<ProfileLibraryScreen *>(ud)->openDetail(index);
  }
  static void list_edge(int dir, void *ud) {
    static_cast<ProfileLibraryScreen *>(ud)->onPageEdge(dir);
  }
  static void back_evt(lv_event_t *e) {
    static_cast<ProfileLibraryScreen *>(lv_event_get_user_data(e))->back();
  }
  static void new_evt(lv_event_t *e) {
    static_cast<ProfileLibraryScreen *>(lv_event_get_user_data(e))->onNew();
  }
  static void edit_evt(lv_event_t *e) {
    static_cast<ProfileLibraryScreen *>(lv_event_get_user_data(e))->onEdit();
  }
  static void dup_evt(lv_event_t *e) {
    static_cast<ProfileLibraryScreen *>(lv_event_get_user_data(e))->onDuplicate();
  }
  static void rename_evt(lv_event_t *e) {
    static_cast<ProfileLibraryScreen *>(lv_event_get_user_data(e))->onRenameRequested();
  }
  static void rename_ok(lv_event_t *e) {
    auto *s = static_cast<ProfileLibraryScreen *>(lv_event_get_user_data(e));
    s->onRenameCommit(lv_textarea_get_text(s->rename_ta_));
  }
  static void delete_evt(lv_event_t *e) {
    static_cast<ProfileLibraryScreen *>(lv_event_get_user_data(e))->onDeleteRequested();
  }
  static void confirm_delete(void *ud) {
    static_cast<ProfileLibraryScreen *>(ud)->onDeleteConfirmed();
  }
  static void cancel_delete(void *ud) { static_cast<ProfileLibraryScreen *>(ud)->back(); }
  static void sort_toggle(lv_event_t *e) {
    static_cast<ProfileLibraryScreen *>(lv_event_get_user_data(e))->toggleSortAndReload();
  }
};

// --- Lifecycle ---

void ProfileLibraryScreen::begin(lv_obj_t *parent, ManagementClient &client,
                                 const OvenModel &model) {
  parent_ = parent;
  client_ = &client;
  model_ = &model;
  pick_ = false; // the Profiles branch manages profiles; pick mode is beginPick's
  showChooser(); // the one view-model is init'd per mode in openMode()
}

void ProfileLibraryScreen::beginPick(lv_obj_t *parent, ManagementClient &client, RecipeMode mode,
                                     const OvenModel &model) {
  parent_ = parent;
  client_ = &client;
  model_ = &model;
  pick_ = true;   // read-only selection; skip the chooser, hide management verbs
  openMode(mode); // straight into the mode's list (MRU-ordered by default — see openMode)
}

void ProfileLibraryScreen::setExitHandler(void (*cb)(void *), void *user_data) {
  on_exit_ = cb;
  exit_ud_ = user_data;
}

void ProfileLibraryScreen::setPickHandler(void (*cb)(void *, const ProfileDraft &),
                                          void *user_data) {
  on_pick_ = cb;
  pick_ud_ = user_data;
}

void ProfileLibraryScreen::publishNav(int nav_request) {
  lv_subject_set_int(&subj_nav_request, nav_request);
}

// --- Shared building blocks ---

void ProfileLibraryScreen::clearParent() {
  lv_obj_clean(parent_);
  title_label_ = nullptr; // lv_obj_clean deleted it; never leave a dangling handle behind
}

void ProfileLibraryScreen::configParent() {
  theme::apply_screen(parent_);
  lv_obj_set_flex_flow(parent_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(parent_, theme::PAD_S, 0);
  lv_obj_set_style_pad_row(parent_, theme::GAP, 0);
}

void ProfileLibraryScreen::buildHeader(const char *title) {
  lv_obj_t *header = lv_obj_create(parent_);
  theme::apply_panel(header);
  lv_obj_set_width(header, lv_pct(100));
  lv_obj_set_height(header, theme::SECONDARY_H);
  lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_gap(header, theme::PAD_M, 0);

  lv_obj_t *back = lv_button_create(header);
  theme::apply_secondary(back);
  lv_obj_set_height(back, lv_pct(100));
  lv_obj_t *back_label = lv_label_create(back);
  lv_label_set_text(back_label, LV_SYMBOL_LEFT " Back");
  lv_obj_center(back_label);
  lv_obj_add_event_cb(back, ProfileThunks::back_evt, LV_EVENT_CLICKED, this);

  lv_obj_t *title_label = lv_label_create(header);
  lv_label_set_text(title_label, title);
  title_label_ = title_label; // kept so a background refresh can mark the header in place
  lv_obj_set_flex_grow(title_label, 1);
  lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);

  // The library lists/opens profiles that live on the controller now (§7/§9), so surface a dropped
  // link here as everywhere. buildDetail() builds its own header and adds the banner itself.
  create_link_banner(parent_);
}

// --- Navigation ---

void ProfileLibraryScreen::showChooser() {
  pending_ = Pending::None; // dropping any in-flight interest; a new op overwrites the client
  page_ = Page::Chooser;
  buildChooser();
}

void ProfileLibraryScreen::openMode(RecipeMode mode) {
  mode_ = mode;
  current_ = &vm_;
  vm_.init(*client_, phase_codec::modeToWire(mode), *model_);
  current_->setFahrenheit(lv_subject_get_int(&subj_units) != 0);
  // The Setup picker defaults to most-recently-used (§23, the run-start use case); the manage-
  // library base stays alphabetical. The controller sorts (§2/§6a) — this only sets which order
  // the requestList() below asks for.
  current_->setMruSort(pick_);
  selected_ = 0;
  restore_ = Restore::Index; // a fresh mode starts at the top, not on the other mode's anchor
  pending_anchor_[0] = '\0';
  return_page_ = Page::Chooser;

  // Render the prefetched window first, if we have one: the rows appear immediately instead of
  // after a ~400 ms round-trip. Then revalidate in the background — the cache can be stale (the
  // library is shared, and §21 will let profiles arrive over WiFi), so what the operator sees is
  // "instantly right, and corrected within a round-trip" rather than "correct after a wait".
  // MRU is deliberately not served from cache: its whole point is recency, and the picker's order
  // changes every time a run starts.
  const Prefetch *pf = prefetchSlot(mode);
  if (pf != nullptr && pf->valid && !pick_) { // pick_ == MRU order (set just above)
    current_->adoptList(pf->list);
    page_ = Page::List;
    buildList();
  }
  // Fetch this mode's library from the controller (§9). The reply lands in poll() → buildList().
  want_list_ = true;
  startListRequest();
}

// Issue the pending list request if the shared client is free, and enter the wait. Leaves
// want_list_ set when it cannot, so poll() retries.
//
// A busy client is NORMAL here, not a failure: the background settings sync and the prefetch both
// use it. Treating that as "couldn't reach the controller" was a latent bug — tapping into Profiles
// while a settings sync was in flight showed the error page even though the link was fine.
void ProfileLibraryScreen::startListRequest() {
  if (!want_list_ || current_ == nullptr) {
    return;
  }
  const bool ok = restore_ == Restore::Name ? current_->requestListAnchored(pending_anchor_)
                                            : current_->requestList();
  if (!ok) {
    // Client busy. Show *something* while we wait for it, unless rows are already up.
    if (page_ != Page::List) {
      page_ = Page::Loading;
      buildLoading("Loading profiles...");
    }
    return;
  }
  want_list_ = false;
  beginListFetch();
}

void ProfileLibraryScreen::openDetail(int index) {
  selected_ = index;
  return_page_ = Page::List;
  want_detail_ = index;
  want_list_ = false; // the operator moved on; a pending background revalidate is now moot
  startDetailRequest();
}

// Issue the pending detail fetch if the shared client is free; otherwise wait on the Loading page
// and let poll() retry. Same reasoning as startListRequest: with a background prefetch and the
// settings sync both using the client, "busy" is a normal transient, not a link failure.
void ProfileLibraryScreen::startDetailRequest() {
  if (want_detail_ < 0 || current_ == nullptr) {
    return;
  }
  page_ = Page::Loading;
  buildLoading("Loading...");
  if (current_->requestDetail(static_cast<size_t>(want_detail_))) {
    pending_ = Pending::Detail;
    want_detail_ = -1;
  }
}

// The cache slot for a mode, allocating the pair on first use. Returns nullptr if the heap says no,
// in which case every caller degrades to "no prefetch" rather than failing.
ProfileLibraryScreen::Prefetch *ProfileLibraryScreen::prefetchSlot(RecipeMode m) {
  if (prefetch_ == nullptr) {
    prefetch_ = new (std::nothrow) Prefetch[2];
  }
  return prefetch_ == nullptr ? nullptr : &prefetch_[modeIdx(m)];
}

void ProfileLibraryScreen::invalidatePrefetch() {
  if (prefetch_ == nullptr) {
    return;
  }
  prefetch_[0].valid = false;
  prefetch_[1].valid = false;
}

// Speculatively pull window 0 of a mode we have not cached yet. Strictly lowest priority: it only
// moves when the shared client is idle AND this screen wants nothing itself, so it can never delay
// a request the operator is waiting on. The composition root gates it further, to the screens where
// the operator is plainly not in the library.
void ProfileLibraryScreen::servicePrefetch() {
  if (client_ == nullptr || pending_ != Pending::None || want_list_ || client_->busy() ||
      !client_->idle()) {
    return;
  }
  for (RecipeMode m : {RecipeMode::Cure, RecipeMode::Reflow}) {
    const Prefetch *slot = prefetchSlot(m);
    if (slot == nullptr) {
      return; // no heap for the cache; run without one
    }
    if (slot->valid) {
      continue;
    }
    // Alphabetical only. MRU is a recency order that a run-start ProfileTouch reorders, so a
    // cached MRU window would be wrong precisely when it is used (the Setup picker).
    if (client_->requestList(phase_codec::modeToWire(m), oven_ProfileSort_PROFILE_SORT_ALPHA)) {
      prefetch_mode_ = m;
      pending_ = Pending::Prefetch;
    }
    return; // one at a time; the other mode gets the next idle turn
  }
}

// Drive the async state machine (called every loop after client.service()).
void ProfileLibraryScreen::poll() {
  if (client_ == nullptr || client_->busy()) {
    return;
  }
  if (pending_ == Pending::None) {
    // A request that lost the race for the shared client; it is free now. Detail first: it is
    // user-initiated and they are staring at a Loading page, whereas a list refresh is background.
    if (want_detail_ >= 0) {
      startDetailRequest();
    } else {
      startListRequest();
    }
    return;
  }
  if (client_->failed()) {
    const bool wasPrefetch = pending_ == Pending::Prefetch;
    pending_ = Pending::None;
    client_->clear();
    if (wasPrefetch) {
      return; // a speculative fetch failing is not something to show anyone
    }
    page_ = Page::Error;
    buildError();
    return;
  }
  // Ready — dispatch on what we asked for.
  switch (pending_) {
  case Pending::List:
    current_->adoptList(client_->list());
    // Window 0 of the current sort is exactly what a prefetch would have fetched, so keep it: a
    // trip out to the chooser and back then costs nothing. Only ALPHA is cached (see openMode).
    if (current_->offset() == 0 && !pick_) {
      if (Prefetch *slot = prefetchSlot(mode_)) {
        slot->list = client_->list();
        slot->valid = true;
      }
    }
    client_->clear();
    pending_ = Pending::None;
    page_ = Page::List;
    buildList();
    break;
  case Pending::Prefetch:
    // Completed while the operator is elsewhere: stash it and touch NOTHING on screen. parent_
    // belongs to whatever screen is actually showing, so building here would draw into it.
    if (Prefetch *slot = prefetchSlot(prefetch_mode_)) {
      slot->list = client_->list();
      slot->valid = true;
    }
    client_->clear();
    pending_ = Pending::None;
    break;
  case Pending::Detail:
    current_->adoptDetail(client_->profile());
    client_->clear();
    pending_ = Pending::None;
    if (pick_) {
      // Pick mode (Setup → Load, §19/C6): the fetch was only to assemble the run draft — hand it
      // straight to the caller (→ Confirm, which shows the one preview graph). No detail page.
      onPickUse();
    } else {
      page_ = Page::Detail;
      buildDetail();
    }
    break;
  case Pending::Action: {
    // A mutation (dup/rename/delete) succeeded — re-list to reflect it. ANCHORED on the row the
    // action was about (§23): the sort can move it to a different window entirely, so asking for
    // "the same offset" would show the wrong rows, and keeping the old `selected_` index would
    // highlight whatever slid into that slot. The anchor makes the controller return the window
    // that actually holds it, and Restore::Name puts the highlight back on it.
    client_->clear();
    pending_ = Pending::None;
    invalidatePrefetch(); // the library just changed; a cached window would render the old one
    want_list_ = true;
    startListRequest(); // stays on "Working..." until the fresh list arrives
    break;
  }
  case Pending::None:
    break;
  }
}

// The list title, with a trailing marker while a window is being fetched underneath it.
const char *ProfileLibraryScreen::listTitle(bool busy) const {
  const bool cure = mode_ == RecipeMode::Cure;
  if (busy) {
    return cure ? "Cure profiles ..." : "Reflow profiles ...";
  }
  return cure ? "Cure profiles" : "Reflow profiles";
}

// Enter the wait for a list reply.
//
// A fetch that has rows to show already does NOT tear the screen down: the previous window stays
// rendered and the header takes a "..." marker. The full-screen "Loading profiles..." is reserved
// for a fetch with genuinely nothing behind it (a cold open with no prefetch). Swapping a rendered
// list for a centred spinner reads as a page transition, which made a ~400 ms round-trip feel far
// slower than it is — and on a page turn it also throws away the rows the operator was reading.
void ProfileLibraryScreen::beginListFetch() {
  pending_ = Pending::List;
  const bool haveRows = page_ == Page::List && current_ != nullptr && current_->count() > 0;
  if (haveRows && title_label_ != nullptr) {
    lv_label_set_text(title_label_, listTitle(/*busy=*/true));
    return; // stay on Page::List with the current rows; buildList() replaces them on arrival
  }
  page_ = Page::Loading;
  buildLoading("Loading profiles...");
}

void ProfileLibraryScreen::buildLoading(const char *msg) {
  clearParent();
  configParent();
  buildHeader(mode_ == RecipeMode::Cure ? "Cure profiles" : "Reflow profiles");
  lv_obj_t *lbl = lv_label_create(parent_);
  lv_label_set_text(lbl, msg);
  lv_obj_set_flex_grow(lbl, 1);
  lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
  theme::apply_caption(lbl);
}

void ProfileLibraryScreen::buildError() {
  clearParent();
  configParent();
  buildHeader("Profiles");
  lv_obj_t *lbl = lv_label_create(parent_);
  // The one thing a UI remote must say when the link is down (§22 wording is for run faults; this
  // is a benign idle-context management failure, so a plain caption, not the red modal).
  lv_label_set_text(lbl, "Couldn't reach the controller.\nCheck the link and try again.");
  lv_obj_set_width(lbl, lv_pct(100));
  lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_flex_grow(lbl, 1);
  lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
  theme::apply_caption(lbl);
}

void ProfileLibraryScreen::back() {
  switch (page_) {
  case Page::ConfirmDelete:
    page_ = Page::Detail;
    buildDetail(); // rebuild detail (clears the overlay dialog with it)
    break;
  case Page::Rename:
    page_ = Page::Detail;
    buildDetail(); // cancel the rename — back to the profile detail
    break;
  case Page::Detail:
    page_ = Page::List;
    buildList();
    break;
  case Page::List:
    // Pick mode has no chooser (Setup already chose the mode) — Back exits to Setup.
    if (pick_) {
      if (on_exit_ != nullptr) {
        on_exit_(exit_ud_);
      }
    } else {
      showChooser();
    }
    break;
  case Page::Loading:
  case Page::Error:
    // Abandon the in-flight/failed request and return to the last stable page. A late reply is
    // ignored (pending_ cleared) and a subsequent request just overwrites the client.
    pending_ = Pending::None;
    if (return_page_ == Page::List) {
      page_ = Page::List;
      buildList();
    } else if (pick_) {
      if (on_exit_ != nullptr) {
        on_exit_(exit_ud_); // pick mode's stable page is the list; a pre-list failure exits
      }
    } else {
      showChooser();
    }
    break;
  case Page::Chooser:
    if (on_exit_ != nullptr) {
      on_exit_(exit_ud_);
    }
    break;
  }
}

// --- Chooser (Cure | Reflow) ---

namespace {
// A big Home-style mode tile (§14 apply_mode_tile), `screen` as the click user_data.
lv_obj_t *make_mode_tile(lv_obj_t *parent, const char *text, lv_event_cb_t on_click, void *screen) {
  lv_obj_t *btn = lv_button_create(parent);
  theme::apply_mode_tile(btn);
  lv_obj_t *label = lv_label_create(btn);
  lv_label_set_text(label, text);
  // The two-word labels ("UV CURE PROFILES") can exceed a tile's width on the narrow 2.8" side-by-
  // side layout, so wrap + centre rather than clip.
  lv_obj_set_width(label, lv_pct(90));
  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(label);
  lv_obj_add_event_cb(btn, on_click, LV_EVENT_CLICKED, screen);
  return btn;
}
} // namespace

void ProfileLibraryScreen::buildChooser() {
  clearParent();
  configParent();
  buildHeader("Profiles");

  // Exactly two profile types, so this is two big Home-style tiles (a direct tap), not a ▲/▼ list.
  // Each tile fetches that mode's library from the controller (§7/§9 — the profiles are no longer
  // CYD-local), so both are link-gated: greyed + non-clickable when the link is down, like Home's
  // run tiles, with the banner above saying why. (Gated below, after the tiles are built.)
  lv_obj_t *modes = lv_obj_create(parent_);
  theme::apply_row(modes);
  lv_obj_set_width(modes, lv_pct(100));
  lv_obj_set_flex_grow(modes, 1);
  lv_obj_set_flex_flow(modes, panel::kPortrait ? LV_FLEX_FLOW_COLUMN : LV_FLEX_FLOW_ROW);

  lv_obj_t *cure = make_mode_tile(modes, "UV CURE PROFILES", ProfileThunks::choose_cure, this);
  lv_obj_t *reflow = make_mode_tile(modes, "REFLOW PROFILES", ProfileThunks::choose_reflow, this);
  for (lv_obj_t *tile : {cure, reflow}) {
    lv_obj_set_flex_grow(tile, 1);
    if (panel::kPortrait) {
      lv_obj_set_width(tile, lv_pct(100));
    } else {
      lv_obj_set_height(tile, lv_pct(100));
    }
    lv_obj_bind_flag_if_eq(tile, &subj_link_state, LV_OBJ_FLAG_CLICKABLE, LINK_OK);
    lv_obj_bind_state_if_not_eq(tile, &subj_link_state, LV_STATE_DISABLED, LINK_OK);
  }
}

// --- Mode-scoped library list ---

void ProfileLibraryScreen::buildList() {
  clearParent();
  configParent();
  // Pick mode is titled by intent ("Load a profile"), not by which library it happens to be.
  buildHeader(pick_ ? "Load a profile"
                    : (mode_ == RecipeMode::Cure ? "Cure profiles" : "Reflow profiles"));

  // Pick mode: a full-width sort toggle above the list (§23). Its label is the CURRENT order, so
  // the operator reads what they have, and tapping flips it and re-fetches (the controller sorts).
  if (pick_) {
    lv_obj_t *sort = lv_button_create(parent_);
    theme::apply_secondary(sort);
    lv_obj_set_width(sort, lv_pct(100));
    lv_obj_set_height(sort, theme::SECONDARY_H);
    lv_obj_t *sort_lbl = lv_label_create(sort);
    // Label reads "Sort: <current order>" — plain text (the subsetted font has no loop/refresh
    // glyph; a tofu box is worse than none). Tapping flips it and re-fetches.
    lv_label_set_text(sort_lbl, current_->mruSort() ? "Sort: Recently used" : "Sort: A - Z");
    lv_obj_center(sort_lbl);
    lv_obj_add_event_cb(sort, ProfileThunks::sort_toggle, LV_EVENT_CLICKED, this);
  }

  const size_t n = current_->count();
  SelectableListItem items[ProfileLibraryViewModel::kMaxRows];
  for (size_t i = 0; i < n; ++i) {
    // Borrowed pointers into the VM's buffers (they outlive this list — VM is a member). Pick mode
    // opens the same detail preview; its verb reads "Use" since selecting it starts a run.
    items[i] = SelectableListItem{current_->rowLabel(i), current_->rowValue(i), true,
                                  pick_ ? "Use" : "Open"};
  }
  list_model_.init(items, static_cast<int>(n), /*wrap=*/false);
  list_model_.setOpenHandler(ProfileThunks::list_open, this);
  // This list is a WINDOW onto the controller's library (§23 paging), so ▲/▼ at the loaded ends
  // must fetch the adjacent window rather than disable. init() cleared both seams, so re-declare
  // them here alongside the Open handler.
  list_model_.setEdgeHandler(ProfileThunks::list_edge, this);
  list_model_.setMore(current_->morePrev(), current_->moreNext());

  // The manage-library "+ New" makes no sense while picking one to run — pick mode has no leading
  // action (a plain 3-button footer).
  const LeadingAction new_action =
      pick_ ? LeadingAction{} : LeadingAction{"+ New", ProfileThunks::new_evt, this};
  SelectableList ui = create_selectable_list(parent_, list_model_, new_action);

  // Whatever the outcome, this list has consumed the restore intent — clear it before either
  // branch. Leaving it set on the empty path would apply a stale anchor to the NEXT list built
  // (e.g. after deleting the only profile, then creating one), highlighting by a name that has
  // nothing to do with it.
  const Restore restore = restore_;
  restore_ = Restore::Index;

  if (n == 0) {
    pending_anchor_[0] = '\0';
    // §23 empty state. Shouldn't happen once stock seeds ship, but a fresh flash has no profiles.
    lv_obj_t *empty = lv_label_create(ui.list);
    lv_label_set_text(empty, "No profiles - New to create one"); // ASCII dash (font has no em dash)
    lv_obj_set_width(empty, lv_pct(100));
    lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
    theme::apply_caption(empty);
  } else {
    // Place the highlight per what it MEANT, not per its old row number — the window may have
    // scrolled or the sort may have moved the row (see Restore).
    int want = selected_;
    switch (restore) {
    case Restore::Name: {
      const int found = current_->indexOfName(pending_anchor_);
      want = found >= 0 ? found : 0; // gone (deleted, or a failed rename) → top of the window
      break;
    }
    case Restore::First:
      want = 0;
      break;
    case Restore::Last:
      want = static_cast<int>(n) - 1;
      break;
    case Restore::Index:
      break;
    }
    selected_ = want < static_cast<int>(n) ? want : 0;
    list_model_.select(selected_);
    pending_anchor_[0] = '\0';
  }
}

// ▲/▼ at a loaded end of the window: pull in the adjacent one (§23). The highlight lands on the
// abutting row — first row when paging down, last when paging up — so a continuous press walks the
// library without the selection jumping.
//
// Deliberately on demand rather than prefetched: a window is ~16 rows over a 115200 link, ~40-70 ms
// behind the Loading state this screen already has, and prefetching would mean holding a second
// request in flight against a single-outstanding client (§9). Revisit only if it reads as laggy.
void ProfileLibraryScreen::onPageEdge(int dir) {
  if (current_ == nullptr || pending_ != Pending::None) {
    return; // a reply is already in flight; ignore the press rather than queue a second request
  }
  if (!current_->requestAdjacent(dir)) {
    return; // no such window (we were at a real end) — nothing to do, keep the current view
  }
  restore_ = dir < 0 ? Restore::Last : Restore::First;
  beginListFetch(); // keeps the current rows up while the adjacent window is fetched
}

// --- Profile detail / actions ---

namespace {
// One action button in the detail footer. `enabled == false` renders it greyed + non-clickable
// (stock Delete). `this_screen` is the click user_data.
lv_obj_t *action_button(lv_obj_t *row, const char *text, lv_event_cb_t cb, void *this_screen,
                        bool enabled) {
  lv_obj_t *btn = lv_button_create(row);
  theme::apply_secondary(btn);
  lv_obj_set_flex_grow(btn, 1);
  lv_obj_set_height(btn, lv_pct(100));
  lv_obj_t *label = lv_label_create(btn);
  lv_label_set_text(label, text);
  lv_obj_center(label);
  if (enabled) {
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, this_screen);
    // Every detail action (Delete/Rename/Clone/Edit) issues a management request or opens the async
    // editor (§9), so gate it on a healthy link — greyed + non-clickable when down, matching Home's
    // run-tile gate; the banner above says why. Re-enables reactively on reconnect.
    lv_obj_bind_flag_if_eq(btn, &subj_link_state, LV_OBJ_FLAG_CLICKABLE, LINK_OK);
    lv_obj_bind_state_if_not_eq(btn, &subj_link_state, LV_STATE_DISABLED, LINK_OK);
  } else {
    lv_obj_set_state(btn, LV_STATE_DISABLED, true);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_CLICKABLE);
  }
  return btn;
}
} // namespace

void ProfileLibraryScreen::buildDetail() {
  clearParent();
  configParent();

  // Header: Back + profile name + mode word (word, never colour alone — §14).
  lv_obj_t *header = lv_obj_create(parent_);
  theme::apply_panel(header);
  lv_obj_set_width(header, lv_pct(100));
  lv_obj_set_height(header, theme::SECONDARY_H);
  lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_gap(header, theme::PAD_M, 0);

  lv_obj_t *back = lv_button_create(header);
  theme::apply_secondary(back);
  lv_obj_set_height(back, lv_pct(100));
  lv_obj_t *back_label = lv_label_create(back);
  lv_label_set_text(back_label, LV_SYMBOL_LEFT " Back");
  lv_obj_center(back_label);
  lv_obj_add_event_cb(back, ProfileThunks::back_evt, LV_EVENT_CLICKED, this);

  lv_obj_t *name = lv_label_create(header);
  lv_label_set_text(name, current_->name(selected_));
  lv_obj_set_flex_grow(name, 1);
  lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);

  lv_obj_t *badge = lv_label_create(header);
  lv_label_set_text(badge, mode_ == RecipeMode::Cure ? "Cure" : "Reflow");
  lv_obj_set_style_text_color(badge, theme::col(theme::ACCENT), 0);

  create_link_banner(parent_); // custom header, so the banner is added explicitly (see buildHeader)

  // Read-only §12 curve preview (requested vs achievable) with phase separators, phase names, axis
  // ticks, and UV shading. Local arrays — the widget copies.
  profile_facts::CurvePoint req[profile_facts::kMaxCurvePoints];
  profile_facts::CurvePoint over[profile_facts::kMaxCurvePoints];
  float bounds[profile_facts::kMaxCurvePhases];
  profile_facts::TimeSpan uv[kMaxPhases];
  const size_t nr = current_->sampleRequested(req, profile_facts::kMaxCurvePoints);
  const size_t no = current_->sampleOvershoot(over, profile_facts::kMaxCurvePoints);
  const size_t nb = current_->samplePhaseBoundaries(bounds, profile_facts::kMaxCurvePhases);
  const size_t nuv = current_->sampleUvSpans(uv, kMaxPhases);
  // Phase labels: one per AUTHORED phase (its stored name). The implicit passive cool-down the
  // samplers append when the run ends hot (implicit_cool.h, §6) is a system safety phase, not
  // operator-authored — it gets no label; its separator and the end-time label carry the right
  // edge.
  char namebuf[profile_facts::kMaxCurvePhases][kPhaseNameCap];
  const char *names[profile_facts::kMaxCurvePhases];
  const size_t authored = current_->phaseNames(namebuf, profile_facts::kMaxCurvePhases);
  for (size_t i = 0; i < authored && i < nb; ++i) {
    names[i] = namebuf[i];
  }
  // Cool phase starts where the last authored phase ends — lets the widget compress the long
  // passive cool-down of a hot reflow rather than squishing the authored phases into the left edge.
  const float cool_start = (nb > authored && authored >= 1) ? bounds[authored - 1] : -1.0f;
  ProfileCurveData cd;
  cd.requested = req;
  cd.n_requested = nr;
  cd.overshoot = over;
  cd.n_overshoot = no;
  cd.boundaries = bounds;
  cd.n_boundaries = nb;
  cd.uv_spans = uv;
  cd.n_uv_spans = nuv;
  cd.phase_names = names;
  cd.n_phase_names = authored; // authored phases only — the implicit cool is unlabelled
  cd.cool_start = cool_start;
  cd.uncalibrated = false; // shown as a warning bar below the graph instead of a label on it
  create_profile_curve(parent_, cd);

  // Uncalibrated warning as an amber bar under the graph (mirrors the editor), rather than a label
  // drawn over the curve.
  if (current_->uncalibrated()) {
    lv_obj_t *banner = lv_obj_create(parent_);
    theme::apply_alert(banner, theme::WARN);
    lv_obj_set_width(banner, lv_pct(100));
    lv_obj_set_height(banner, theme::BANNER_H);
    lv_obj_t *lbl = lv_label_create(banner);
    lv_label_set_text(lbl, "UNCALIBRATED");
    lv_obj_center(lbl);
  }

  // Key facts: "peak 245° · ~6:10 · 4 phases".
  const profile_facts::ProfileFacts f = current_->facts();
  char peak[16];
  char dur[16];
  char facts[48];
  profile_facts::formatPeak(f.peakC, current_->fahrenheit(), peak, sizeof(peak));
  profile_facts::formatDuration(f.totalSeconds, dur, sizeof(dur));
  std::snprintf(facts, sizeof(facts), "%s \xC2\xB7 %s \xC2\xB7 %u phases", peak, dur,
                static_cast<unsigned>(f.phaseCount));
  lv_obj_t *facts_label = lv_label_create(parent_);
  lv_label_set_text(facts_label, facts);
  theme::apply_caption(facts_label);

  // (Pick mode never reaches buildDetail — poll() hands the run draft straight to Confirm, §19/C6.)

  // Action row (managing profiles only — running one is a separate path, Home → UV Cure / Reflow →
  // Setup, §19, so there is no Load here). A STOCK profile is read-only: it shows just Delete
  // (greyed) · Clone — Clone is the way to fork it into an editable user copy, so a separate
  // "Save as" would be redundant. A USER profile shows Delete · Rename · Clone · Edit; Edit is
  // rightmost (the most common action, under the finger that just tapped Open), the destructive
  // Delete at the far left. Rename/Edit are hidden for stock (nothing to rename or edit in place).
  const bool user = !current_->rowStock(selected_);
  lv_obj_t *row = lv_obj_create(parent_);
  theme::apply_row(row);
  lv_obj_set_width(row, lv_pct(100));
  lv_obj_set_height(row, theme::SECONDARY_H);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  action_button(row, "Delete", ProfileThunks::delete_evt, this, current_->canDelete(selected_));
  if (user) {
    action_button(row, "Rename", ProfileThunks::rename_evt, this, true);
  }
  action_button(row, "Clone", ProfileThunks::dup_evt, this, true);
  if (user) {
    action_button(row, "Edit", ProfileThunks::edit_evt, this, true);
  }
}

// --- Detail actions ---

void ProfileLibraryScreen::onNew() {
  publishNav(NAV_PROFILE_NEW); // → editor (C5); observed only by tests until it lands
}

void ProfileLibraryScreen::onEdit() {
  publishNav(NAV_PROFILE_EDIT); // → editor / Save-as (C5)
}

void ProfileLibraryScreen::onDuplicate() {
  return_page_ = Page::List;
  // Clone on the controller ("<name> copy", deconflicted against the cached window); poll()
  // re-lists anchored on the SOURCE row. The copy sorts next to it alphabetically, so both land in
  // view, and the highlight stays on the profile the operator was looking at rather than following
  // the new one somewhere they did not ask to go.
  setAnchor(current_->name(static_cast<size_t>(selected_)));
  if (current_->requestDuplicate(static_cast<size_t>(selected_))) {
    page_ = Page::Loading;
    pending_ = Pending::Action;
    buildLoading("Working...");
  } else {
    page_ = Page::Error;
    buildError();
  }
}

void ProfileLibraryScreen::onRenameRequested() {
  page_ = Page::Rename;
  buildRename();
}

void ProfileLibraryScreen::onRenameCommit(const char *text) {
  // Rename on the controller; poll() re-lists on success (the name changed, so the highlight's row
  // may have moved under the alphabetical sort). A refusal (empty/invalid/taken name) comes back as
  // a NAK → the error page; Back returns to the list to try again.
  // Reject an empty or already-taken name client-side so the operator stays on the keyboard to pick
  // another (the profile keeps its old name); the controller validates everything else.
  if (text == nullptr || text[0] == '\0' || current_->nameExists(text)) {
    return;
  }
  return_page_ = Page::List;
  // Anchor on the NEW name: a rename re-sorts the row, potentially into a different window, and
  // this is what follows it there. (Before paging this screen kept the old index and could end up
  // highlighting whichever profile slid into that slot.)
  setAnchor(text);
  if (current_->requestRename(static_cast<size_t>(selected_), text)) {
    page_ = Page::Loading;
    pending_ = Pending::Action;
    buildLoading("Working...");
  }
  // else: client busy — stay on the keyboard (the profile keeps its old name).
}

void ProfileLibraryScreen::buildRename() {
  clearParent();
  configParent();
  buildHeader("Rename");

  rename_ta_ = lv_textarea_create(parent_);
  lv_textarea_set_one_line(rename_ta_, true);
  lv_textarea_set_max_length(rename_ta_, static_cast<uint32_t>(kProfileNameCap - 1));
  lv_textarea_set_placeholder_text(rename_ta_, "Profile name");
  lv_textarea_set_text(rename_ta_, current_->name(selected_)); // prefilled with the current name
  lv_obj_set_width(rename_ta_, lv_pct(100));

  // A flex-grow spacer pins the keyboard to the bottom, the field under the header (editor idiom).
  lv_obj_t *spacer = lv_obj_create(parent_);
  lv_obj_remove_style_all(spacer);
  lv_obj_set_width(spacer, lv_pct(100));
  lv_obj_set_flex_grow(spacer, 1);
  lv_obj_remove_flag(spacer, LV_OBJ_FLAG_CLICKABLE);

  name_kb::create(parent_, rename_ta_, ProfileThunks::rename_ok, this);
}

void ProfileLibraryScreen::onDeleteRequested() {
  page_ = Page::ConfirmDelete;
  char msg[64];
  std::snprintf(msg, sizeof(msg), "Delete \"%s\"?", current_->name(selected_));
  // Overlay on top of the still-built detail page; cancel rebuilds detail, confirm deletes + lists.
  create_confirm_dialog(parent_, msg, "Delete", ProfileThunks::confirm_delete,
                        ProfileThunks::cancel_delete, this);
}

void ProfileLibraryScreen::onDeleteConfirmed() {
  return_page_ = Page::List;
  // Anchor on the row that will SURVIVE next to this one — the one below, or the one above when
  // deleting the last row — so the cursor lands where the deleted profile was rather than jumping
  // to the top of a library that may be several windows long. If this was the only row, there is no
  // neighbour and the refresh falls back to the current offset, which the controller clamps.
  const size_t sel = static_cast<size_t>(selected_);
  const size_t neighbour = sel + 1 < current_->count() ? sel + 1 : (sel > 0 ? sel - 1 : sel);
  if (neighbour != sel) {
    setAnchor(current_->name(neighbour));
  } else {
    restore_ = Restore::Index;
    pending_anchor_[0] = '\0';
  }
  if (current_->requestRemove(sel)) {
    selected_ = 0; // the row is going away; reset the remembered highlight
    page_ = Page::Loading;
    pending_ = Pending::Action;
    buildLoading("Working...");
  } else {
    page_ = Page::Error;
    buildError();
  }
}

// Record which profile the next adopted list should highlight, and ask for it by name.
void ProfileLibraryScreen::setAnchor(const char *name) {
  if (name == nullptr) {
    restore_ = Restore::Index;
    pending_anchor_[0] = '\0';
    return;
  }
  std::strncpy(pending_anchor_, name, kProfileNameCap - 1);
  pending_anchor_[kProfileNameCap - 1] = '\0';
  restore_ = Restore::Name;
}

// --- Pick mode (§19/C6) ---

void ProfileLibraryScreen::onPickUse() {
  // The detail is loaded (we are on Page::Detail), so assemble the run working copy from it + the
  // selected row and hand it to Setup. Setup navigates away; this screen does not rebuild.
  if (on_pick_ == nullptr || !current_->haveDetail()) {
    return;
  }
  ProfileDraft draft;
  current_->detailToDraft(static_cast<size_t>(selected_), draft);
  on_pick_(pick_ud_, draft);
}

void ProfileLibraryScreen::toggleSortAndReload() {
  current_->toggleSort();
  // Re-fetch in the new order (the controller sorts) — same async path as openMode's list fetch.
  return_page_ = Page::List;
  if (current_->requestList()) {
    page_ = Page::Loading;
    pending_ = Pending::List;
    buildLoading("Loading profiles...");
  } else {
    page_ = Page::Error;
    buildError();
  }
}
