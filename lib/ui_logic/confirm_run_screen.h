// confirm_run_screen — the §19 run-confirmation screen (backlog C6b). The last gate before the one
// irreversible action on the panel: committing a run. Setup → Start lands here with the composed
// run draft; a press-and-hold "arm" (hold_button) is the only way forward, Cancel is a plain tap.
//
// It states specifically what is about to happen (name · peak · duration, from profile_facts), and
// carries the mode-specific safety precondition:
//   - REFLOW: the workpiece thermocouple must be attached and reading plausibly (tcAttached over
//     live telemetry) before HOLD-to-start enables — a run driven off a dangling probe is the
//     classic reflow ruin (§5/§19);
//   - CURE: no precondition line — the UV array is filtered at the door window and the door latches
//     cut the light when it opens, so no eye-hazard caution is needed.
//
// On commit it runs the §9 start handshake against the CydLink: sendRecipe → (Ack) → sendStart →
// (Ack) → setSession + setEnable(true), then a best-effort ProfileTouch so the run's source floats
// to the top of the picker's recency (§23/C6-PR1), then hands off to the Run screen (C7). A Nak or
// timeout drops to a Failed page rather than starting blind.
//
// A self-contained hub-and-spoke controller (the editor's begin*/render/poll split): begin() sets
// the run + compiles the recipe, render() builds the current page, poll() drives the live TC gate
// and the commit state machine. References lib/protocol (CydLink) + lib/app_logic
// (ManagementClient) — host-testable; LVGL-only view. Compiles for firmware and native_ui_cyd /
// native_sim.
#pragma once

#include <cstdint>

#include <lvgl.h>

#include "cyd_link.h"
#include "management_client.h"
#include "oven.pb.h"
#include "oven_cal.h"
#include "profile_draft.h"
#include "recipe_compiler.h" // compileRecipe / CompileResult / Caps

class ConfirmRunScreen {
public:
  enum class Page { Review, Starting, Failed };

  // Arm the screen for `draft` with a controller-unique `session` (0 is "no session", §9). `link`
  // drives the start handshake; `mgmt` sends the recency touch; `model` supplies the preview facts.
  // Compiles the recipe once. All references must outlive this screen.
  void begin(const ProfileDraft &draft, uint32_t session, protocol::CydLink &link,
             ManagementClient &mgmt, const OvenModel &model = oven_cal::kDefaultModel);

  // Build the current page under `parent` (the router build cb; call after begin(), after lv_init()
  // + ui_subjects_init()).
  void render(lv_obj_t *parent);

  // Drive the live gate + commit machine: call every loop after link.service() + mgmt.service(). On
  // Review it re-evaluates the TC gate from the latest telemetry and enables/disables HOLD; while
  // Starting it advances the §9 handshake and fires the commit handler when the run is enabled.
  void poll();

  // Cancel/Back → Setup (the caller rebuilds it). Commit → the Run screen (C7), fired once the run
  // is enabled with the authored draft to hand over.
  void setExitHandler(void (*cb)(void *user_data), void *user_data);
  void setCommitHandler(void (*cb)(void *user_data, const ProfileDraft &draft), void *user_data);

  // Navigation + gesture targets (also directly callable by tests).
  void cancel(); // → exit handler
  void commit(); // HOLD armed → begin the start handshake (a no-op unless ready())
  void back();   // Failed → Review · Review → exit

  // Can the run be committed right now? Link healthy, a hard-valid recipe, and (reflow) the TC
  // attached. Drives the HOLD widget's enabled state.
  bool ready() const;

  // Pure precondition (host-tested + fuzzable): is the workpiece TC attached and reading plausibly?
  // Finite, in a sane physical range, and not implausibly hotter than the chamber walls (an open or
  // dangling probe reads NaN, an open-circuit sentinel, or a value unrelated to the chamber).
  static bool tcAttached(const oven_Telemetry &t);

  Page page() const { return page_; }
  bool committed() const { return commit_ == Commit::Enabled; }

private:
  // The §9 start handshake, one step per poll: send Recipe, await its Ack, send Start, await its
  // Ack, then enable. Nak/timeout on either → Failed.
  enum class Commit { Idle, RecipeSent, StartSent, Enabled, Failed };

  void buildReview();
  void buildStarting();
  void buildFailed();
  void buildHeader(const char *title);
  void configParent();
  void clearParent();

  void beginCommit();      // Review HOLD armed → kick off the handshake
  void driveCommit();      // poll() step of the handshake while Starting
  void refreshGate();      // poll() step: re-evaluate the TC gate + HOLD enable while Review
  void applyReady(bool r); // enable/disable the HOLD widget + repaint the gate line

  bool isReflow() const { return draft_.mode == RecipeMode::Reflow; }
  const oven_Telemetry &telem() const { return link_->lastTelemetry(); }
  bool haveTelem() const { return link_ != nullptr && link_->hasTelemetry(); }

  lv_obj_t *parent_ = nullptr;
  Page page_ = Page::Review;

  ProfileDraft draft_{};
  uint32_t session_ = 0;
  protocol::CydLink *link_ = nullptr;
  ManagementClient *mgmt_ = nullptr;
  const OvenModel *model_ = &oven_cal::kDefaultModel;
  CompileResult recipe_{}; // compiled once in begin() — the upload + the statement facts
  bool touch_sent_ = false;

  Commit commit_ = Commit::Idle;

  // Review-page widgets updated in place by refreshGate() (no full rebuild per telemetry tick).
  lv_obj_t *hold_btn_ = nullptr; // the HOLD-to-start button (gated)
  lv_obj_t *gate_lbl_ = nullptr; // the reflow TC status line (reflow only)
  char gate_buf_[48]{};

  void (*on_exit_)(void *) = nullptr;
  void *exit_ud_ = nullptr;
  void (*on_commit_)(void *, const ProfileDraft &) = nullptr;
  void *commit_ud_ = nullptr;

  friend struct ConfirmThunks;
};
