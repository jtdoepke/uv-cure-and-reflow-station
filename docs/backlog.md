# Backlog — work breakdown for docs/design.md

Each item is ~one PR (some split into 2–3, noted inline). Section refs (§) point
into `docs/design.md`. Items are ordered into **dependency waves**: everything in
a wave is unblocked once the prior waves land, and items *within* a wave are
mostly independent (parallelizable) — the few intra-wave dependencies are called
out with `deps:`. Each item keeps its original track tag: `[A]` protocol/controller,
`[B]` CYD logic, `[C]` UI, `[D]` hardware. `deps:` lists blocking prerequisites
(satisfied done-work omitted); "soft:" is a preferred-but-not-blocking ordering.

The spine to the MVP — **UV cure end-to-end (D5)** — is the critical path. The two
long poles are the **hardware chain D1→D2→D3→D4** and the **safety proof
A4→A8→D3** (§8 is the intended build sequence). Resolve the §10 open question
tagged to a step before starting that step's items.

## Wave 0 — Done (plumbing + first controller/logic layers)

- [x] **M0.1** [A] — Controller env + skeleton + test lane. (§3, §10, §11)
- [x] **M0.2** [A] — nanopb + TinyFrame deps + protobuf codegen step, both envs. (§9, §10)
- [x] **M0.3** [A] — `.proto` v1 + schema-hash build step (`Hello` frozen; `Recipe`/
  `Segment`, `Start`, `Heartbeat`, `Abort`, `Telemetry`, `Ack`/`Nak`, `Done`,
  `Fault` + `faultCode`). (§9, §22)
- [x] **A1** [A] — TinyFrame framing + `ISerialTransport` port + in-memory pipe. (§9)
- [x] **A2** [A] — Reliability layer: heartbeat, seq/ACK-NAK retry, session,
  schema-hash handshake gate. (§9)
  *Amended by **A8** (two bugs its host tests structurally could not see, both now pinned):
  seq restarted at 0 each boot while `SetupResponder` dedups on seq alone, so a rebooted CYD's
  first command was silently swallowed → `ReliableSender::setSeqBase()`; and `Handshake` never
  answered a `Hello`, so whoever heard first went quiet without confirming and the slower board
  never matched → per-boot `Hello.boot_nonce` + answer-on-hear. The tests hid both by booting
  both facades at the same instant. Also **A9**, which built the controller→CYD `Telemetry` half
  of §9's hot path that fell between A2 and C7.*
- [x] **A3** [A] — `HeaterActuator` time-proportioning class. (§11)

## New ports to create (not standalone items — build each with its first consumer)

Several waves depend on ports that don't exist yet. Each is one pure interface in
`lib/control_port/` (or a CYD storage port) + a `test/helpers` fake, matching the
existing `IClock`/`IHeaterSwitch` idiom:

- ~~**Temp-input** (control-sensor readback; §11 `IThermocouples`)~~ *Landed with A6:
  `lib/control_port/IThermocouples.h` (`TcReading {celsius, fault}`; `workpiece()` + a wall array
  that is both the cure control sensor and A4b's L3 high-limit input) plus `FakeThermocouples`
  (`test/helpers`). Consumed by A5/A6/A4b; the real ESP32 TC-front-end adapter is **D4**.*
- **`IContactor`** (energize-to-close; §4) — **A4a**. *Landed with A4a: `lib/control_port/IContactor.h`
  plus `FakeContactor` (`test/helpers`).*
- ~~**`IWatchdog`** (kick + reset-cause readback; §9/§11)~~ *Landed with A8:
  `lib/control_port/IWatchdog.h` (`kick()` + `ResetCause lastResetCause()`) plus `FakeWatchdog`
  (`test/helpers`) and the `Esp32Watchdog` adapter. **A4b** landed the `ResetCause`→`Fault{WATCHDOG}`
  mapping (`SafetySupervisor::noteResetCause`).*
- **Output ports** `IUvOutput` / `IFanOutput` / `IMotorOutput` (§11) — **A6** actuation wiring / **D5**.
- ~~**Storage port** (LittleFS adapter + in-memory fake; §7) — **B4**, shared by **B5**.~~
  *Landed across B5 + B4: **B5** shipped the single-blob settings half (`ISettingsStorage` +
  `FakeSettingsStorage`, `lib/storage_port`); **B4** shipped the keyed profile-blob half —
  `IProfileStorage` (`list`/`read`/`write`/`remove` by name) + `FakeProfileStorage`
  (`test/helpers`) + the `LittleFsProfileStorage` adapter (`src_cyd/`, the repo's first LittleFS
  user).*
- **SD/log sink port** (§7) — **B8**.

## Wave 1 — Ready now (deps satisfied by Wave 0)

- [x] **B2** [B] — shared rate-limit/lag math + stub `oven_cal.h` with default
  constants. deps: none. (§6, §12, §15) *New shared `lib/calibration/`: `thermal_math.h`
  (host-tested `OvenModel` + fan-conditioned rate envelopes, `rampDurationSeconds` ∫dT/rate ETA
  integral, `rateLimitRamp` feasibility, first-order `{a,b,τ}` lag, exposure↔hold, feedforward
  `steadyStateDuty`) + hand-authored `oven_cal.h` placeholders with `CALIBRATED=false` → idealized-
  linear uncalibrated preview (§12). Model threaded by arg (toy-testable; D6·tools regenerates the
  constants only). Safety constants stay in the controller header, not here (§6). Compiles into
  both firmwares (proven under `native_control`). Foundational — unblocks B1/B3/B9, A5's
  feedforward, and the ETA/preview (C7).*
- [x] **B7** [B] — fault table + latching/ack-routing logic (§22) and run-summary
  residual math (§16). deps: none. *One small PR each.*
  *PR1 (§22): `fault_table.h` (code → `{title, guidance, codeName, severity, overTemp}`) +
  `fault_controller.h` (the two-state latch: never auto-dismiss, `+N` supersede, ack-routing).
  Keys off the generated `oven_FaultCode`, so `native_logic_cyd` gained nanopb (mirroring
  `native_control`); a CYD-side copy of the enum would be the drift §9's matched-pair invariant
  exists to prevent. Operator copy lives in `app_logic`, not the view, so §22's wording is
  unit-tested — LINK_LOST's two-clause text verbatim. Both §22 trigger origins enter through
  `FaultController`; the self-raised LINK_LOST is edge-triggered + run-scoped. **Two decisions
  design.md doesn't make** (both presentation-only — the controller has already safed itself —
  and pinned by tests): the **severity order** (`severity` appears once in all of design.md with
  no order defined) HEATER_STUCK > OVERTEMP_* > SENSOR_FAULT/TC_IMPLAUSIBLE > WATCHDOG/INTERNAL/
  unknown > LINK_LOST > TARGET_UNREACHABLE/RUNTIME_EXCEEDED, so a LINK_LOST can't mask a real
  over-temp; and **HOT is chamber-specific** (OVERTEMP_CASE is the §6 electronics sensor and says
  nothing about a touchable chamber).
  PR2 (§16): `deviation_monitor.h` — ONE streaming residual channel; §15's live cue reads
  `deviating()` per tick and §16's summary reads `stats()` at Done off the same object, so
  "same residual math as the live cue" is true by construction, not by two implementations
  agreeing. dt-weighted (a mid-run rate change must not reweight the miss) and clock-free, so
  D6 can replay a log offline for identical numbers. `run_fit.h` — two channels + per-phase
  target checks → `{verdict, cause, advisory}`, implementing §16's discrimination rule
  (dirty estimator → ProjectionModel; clean estimator + missed projection → Oven). Completed
  runs only. Output is an **inline amber advisory, never the red modal** (§22). Lives in
  `app_logic`, not `calibration`: it never touches an `OvenModel` (the projection is B1/C7's
  input), the controller has no use for it, and its thresholds are hand-authored TBD §10
  constants D6's emitter must never clobber.
  Unblocks **C8**'s Fault overlay + Run Summary. All thresholds are unmeasured placeholders —
  §10 "Deviation/drift thresholds" (§8 step 4) owns them. Two items want a human pass: the
  split advisory strings (§16 gives one paragraph for both causes and says the advisory now
  "picks its wording accordingly" without supplying the two texts), and whether the CYD-side
  link timeout belongs here or with C7/B6's link owner.*
- [x] **C1** [C] — `NumericEntry` logic + keypad widget. deps: none. (§26)
  *Host-tested `NumericEntry` state machine (`lib/app_logic`) + on-screen keypad widget &
  view-model (`lib/ui_logic/numeric_keypad*`) with commit/cancel/clamp seams shared with C2/C8.
  Everything numeric routes through it.*
- [x] **C2** [C] — value-stepper widget. deps: none. (§24) *Shared `NumericFieldConfig`
  (`lib/app_logic`, the >20-step editor-routing rule) + the one-value-per-screen stepper
  editor (`lib/ui_logic`) with min/max disable, hold-to-accelerate, amber caution, and
  keypad/commit/cancel seams for C1/C8. Added a per-widget `red_hat_mono_28` for the big
  readout; nested `test_ui_cyd`/`test_logic_cyd` into per-suite dirs like `test_control/*`.*
- [x] **C3** [C] — Home/Status hub. deps: none. *Sets the visual language; do
  early.* (§14) Also laid the reusable MVVM UI foundation — theme tokens, shared
  `lv_subject_t` subjects, view-model/view split — and the Red Hat Mono default font.
  **Live 2026-07-18 (with A10):** the placeholder chamber temp + status dot are now driven by the
  controller's telemetry. The status badge folds run-state AND link health into one dot+word (green
  IDLE / amber RUNNING / amber HOT / red FAULT·NO LINK·SCHEMA — the separate header link readout is
  gone), and the chamber readout's digits are coloured by temperature (white < touch-safe, amber,
  red at the burn line). "HOT" is reserved for an idle chamber at/above touch-safe, so a cold
  chamber mid-run never mislabels.
- [x] **C9** [C] — sleep/wake (§17) + auto-brightness (§18). deps: none. *Ports
  `IAmbientLight`/`IBacklight` (`lib/display_port`) + host-tested `AutoBrightness`
  (filter→curve→additive-bias→hysteresis→ramp→floor/ceiling; sole backlight owner) and
  `SleepController` (idle-only, wake-tap consumed, never sleep non-idle) in `lib/app_logic`;
  ESP32 adapters + `main.cpp` wiring with live bias preview. Curve calibrated + inverted for
  this board's LDR (GPIO34 reads ~0 in room light, climbs in the dark). Verified on hardware.
  Door-open wake deferred until the controller reports door state over telemetry (§9).*
  **Extended 2026-07-18 (with A10):** the sleep gate now shares one predicate with the idle dot
  (`HomeViewModel::atRest`) — the screen may sleep only when idle AND touch-safe, stays awake during
  a run (which also keeps the heartbeat alive) or while the chamber is hot, and a sleeping screen
  relights if the chamber climbs past touch-safe. Door-open wake still waits on the door sensor.
- [x] **A7** [A] — controller-side recipe validation: range checks,
  mode-from-content derivation, NAK reasons (plugs into A2's `ISetupValidator`
  seam). deps: A2. (§4, §9)
  *Host-tested `RecipeValidator` (`lib/control_logic`) implementing `protocol::ISetupValidator`:
  structural + range checks and the §4 content-derived cap selector (any `uv`/`motor` segment
  forces the cure ceiling; a REFLOW-tagged recipe containing them is NAKed), against the
  hand-written `oven_safety.h`. The untrusted `mode` tag is cross-checked, never trusted to pick
  the cap. **Two NAK reasons remain deferred**: `NAK_WORKPIECE_TC_INVALID` (needs the temp-input
  port) and `NAK_ILLEGAL_TRANSITION` (needs A6's run state). Two §10 opens still bear on it — the
  UV/cure absolute ceiling (`CURE_HARD_MAX_C` is a TBD placeholder, gating §8 step 3) and D1's
  fan-motor decision, which fixes the `Segment` fan field's domain.*
- [ ] **D1** [D] — §10 teardown verifications (one investigation task): fan motor
  type, cooling-fan existence, SMPS reuse, humidity-sensor interface, relay board.
  deps: hardware access. *Gates the `Recipe` schema details, the drivers, and all
  of Track D.* (§6, §10)

## Wave 2 — Controller run path + CYD data/store (build on Wave 1 + new ports)

- [x] **A4a** [A] — `SafetySupervisor`: command-timeout + fail-safe defaults +
  contactor policy. deps: A2, A3; new `IContactor`. (§4, §9)
  *Host-tested `SafetySupervisor` (`lib/control_logic`): sole owner of the heater safety-cutoff
  and mains contactor; drives fail-safe (heater OFF, contactor open) on construction and cuts on
  the next tick whenever `ControllerLink::authorized()` drops (pulled TX, reboot, enable-low,
  schema skew, stale >750 ms). Contactor is energize-to-close, closed only while authorized. New
  `IContactor` port + `FakeContactor`. A minimal `trip()`/`clearFault()` latch is the seam A4b's
  L3 clamps and the A6/A7 fault paths hook into (no `FaultCode`/`Fault` emission yet). The
  command-timeout test is the unit-level form of A8's §8-step-1 fail-safe proof. ESP32 adapters +
  `main.cpp` wiring deferred to A8.*
- [x] **A4b** [A] — `SafetySupervisor` L3: setpoint clamp, per-mode over-temp trip,
  stuck-heater plausibility, bounded total runtime, reset-cause → `Fault{WATCHDOG}`.
  deps: A4a, temp-input port, `IWatchdog`; soft: B2 (runtime bound uses `oven_cal`). (§4)
  *Extended `SafetySupervisor` (now `IThermocouples` + `IClock`-injected) with the L3 layer that
  acts on **measured** temp, so it catches a welded SSR the setpoint clamp can't. `armRun(recipe)`
  derives the cap-selector mode from recipe **content** (reusing `oven_safety::deriveMode`) and,
  while armed + authorized, every tick checks: per-mode over-temp trip on an **independent
  high-limit channel** (the hottest non-faulted wall — §4's "own channel, not the control
  sensor"; no knowledge of which channel the executor uses); stuck-heater (measured rise while
  commanded `duty ≈ 0` across a window, off `HeaterActuator::duty()`); bounded runtime; and a
  refuse-to-run-blind `SENSOR_FAULT` when no wall channel is usable. The supervisor is now the
  single fault **aggregator/latch**: the executor's `Output.fault` routes in via a new
  `trip(code)` overload, the L3 checks trip internally, and `faultCode()` exposes the one active
  code. **The runtime bound sidesteps the soft B2 dep**: B1 already baked projected durations into
  `dur_ms`, so the budget is Σ`dur_ms` × margin — summed in 64-bit and saturated to uint32 so a
  raw/adversarial recipe can't overflow it — with no `oven_cal`. `clampSetpoint()` is a **total**
  independent clamp (NaN/±Inf → finite, ≤ hard-max), defense-in-depth over the executor's own.
  Watchdog reset → `Fault{WATCHDOG}` via `noteResetCause()` — **annunciation, not a latch**, since
  the controller already boots safe.
  **Built the controller-side Fault-emit path that didn't exist** (only heartbeat/telemetry
  senders did): new `protocol::FaultSender` mirroring `TelemetrySender` — fires the dedicated
  `Fault` frame on change and re-sends while active so a dropped, un-ACKed frame self-heals; the
  CYD latches on first receipt so repeats are harmless. `telemetry.fault_code` rides continuously
  as the backup channel. This finally gives **B7's `FaultController` a live producer** (§22 modal),
  closing the controller→CYD annunciation pipeline end-to-end (still C8's to render).
  **A4b-shaped fuzzing** joins A5/A6's harnesses: `fuzz/fuzz_safety_supervisor.cpp` drives a raw
  recipe + adversarial driver (temps incl. NaN/Inf, duty, clock jumps, auth/trip/clear) and asserts
  the L3 invariants — latch monotonicity, trip ⇒ safe, `clampSetpoint` totality, run-blind refusal,
  and no UB in `armRun`'s Σ — clean over millions of runs.
  **On-device temp source is deferred to D4** (a `StubThermocouples` reporting no channel; the L3
  checks only run once a run is armed, which rides in with D4's real sensor) — the same
  "logic host-tested, hardware wiring deferred" posture A4a took to A8. **All L3 thresholds are
  §10-open placeholders** (over-temp margin, stuck-heater rise/window, runtime frac) — the §10
  "over-temp-trip / stuck-heater margins + times" item owns them, tuned against real runs (§8 step
  4). Host-tested (`test_safety_supervisor` +10 cases, `test_fault_sender`).*
- [x] **A5** [A] — PID + anti-windup + feedforward hook, tested against a toy
  first-order plant. deps: A3, temp-input port; soft: B2 (feedforward constants). (§5)
  *New `lib/control_logic/heater_control.h`: class `HeaterControl` (PI + an inert D seam),
  conditional-integration anti-windup, and the feedforward-duty hook B2's `steadyStateDuty` feeds.
  Depends only on `IClock`; `update(setpointC, measuredC, ffDuty)` returns a duty clamped to
  `[dutyMin, dutyMax]`, `reset()` clears the integrator. A non-finite (faulted-TC) measurement maps
  to OFF — the loop refuses to control blind. Gains (`kp=0.02, ki=0.002, kd=0`) are §10 placeholders.
  Host-tested against a toy first-order plant (`test_heater_control`) plus a
  `fuzz/fuzz_heater_control.cpp` invariant harness (duty stays in range under adversarial
  gains/trajectories incl. NaN/Inf). **Not yet in the live loop** — the ProfileExecutor→PID→heater
  wiring in `main.cpp` rides in with D4's real temp source (the same logic-tested/hardware-deferred
  posture A4a took to A8, and A4b to D4).*
- [x] **A6** [A] — profile executor: segment sequencing, `RAMP_ASAP` target
  gating, hold-entry gate, per-segment watchdog. deps: temp-input port. *Emits the
  setpoint A5's PID tracks — buildable/testable independently of A5.* (§5)
  *New `lib/control_logic/profile_executor.h`: class `ProfileExecutor`, the run engine that walks an
  accepted `oven_Recipe` and each tick emits `{setpointC, channel states, segIdx, runState, fault,
  safe}` — the setpoint A5 tracks and the `safe`/`fault` seam into `SafetySupervisor::trip()` (A4b).
  Implements the three interps (RAMP_OVER_TIME sweep, RAMP_ASAP target-gated advance, HOLD with the
  reflow hold-entry gate vs the cure dose timer) and the per-segment watchdog (k×projected-dur,
  measured-rate floor, absolute stall cap → `TARGET_UNREACHABLE`) whose constants are §10
  placeholders. Mode-agnostic by construction — the caller selects the control sensor (workpiece in
  reflow, wall in cure). Robust to a raw, pre-validation recipe: non-finite `heat_c` and
  out-of-range `interp` are handled without UB (`interp` read via `protocol::wireEnum`), and the
  emitted setpoint is always finite and clamped to the recipe's own max. **Also shipped the
  temp-input port** this, A5, and A4b share: `IThermocouples` (`lib/control_port`) + `FakeThermocouples`.
  Host-tested (`test_profile_executor`) + a `fuzz/fuzz_executor.cpp` liveness harness (the run always
  leaves RUNNING under any recipe + trajectory). `main.cpp` wiring deferred to D4 as with A5.*
- [x] **B1** [B] — phase→segment compiler: {target, ramp `x`, hold `y`} → generic
  segments; two-tier validation; exposure→hold-seconds math with fallback/labeling.
  deps: B2. (§5, §12)
  *New `lib/app_logic/`: `phase.h` (the editable domain model — `FanMode {Auto,On,Off}` tri-state,
  `RecipeMode`, the `Phase` struct — kept apart from the compiler because C5's editor mutates it),
  `recipe_compiler.h` (`compileRecipe()` lowering a `Phase[]` into an `oven_Recipe`), and B3's
  `fan_resolver.h` (below). Each phase → a ramp segment (`RAMP_OVER_TIME` when x>0, `RAMP_ASAP` when
  x=0 with dur estimated via `rampDurationSeconds` for the ETA/watchdog) + a hold; a degenerate ramp
  (no temperature change) or a hold rounding to 0 ms is omitted — the latter matters because the
  controller NAKs `dur_ms==0`, so every emitted segment carries dur>0. Cure holds compute from
  UV-exposure-per-surface via `beamCoverage`, falling back to raw seconds when the turntable is off
  or there is no coverage, and are labeled estimated when uncalibrated. **Two-tier validation
  (§12):** a *hard* tier that is a strict subset of the controller's `RecipeValidator` (A7) — ≥1
  phase, targets finite + within the passed-in caps, reflow-tag/content consistency, ≤32 segments —
  so an accepted compile never NAKs on upload; and an *amber* tier (rate-limited ramps,
  estimated/fallback holds, heuristic fans, whole-recipe uncalibrated preview) that only warns —
  those stay saveable/uploadable and take their real time via the controller's hold-entry gate. Caps
  and the `OvenModel` are passed by argument (no policy in the compiler), matching `thermal_math.h`.
  Host-tested (`test_recipe_compiler`, 11 cases) **plus a differential fuzz harness**
  (`fuzz/fuzz_compiler.cpp`, env `fuzz_compiler`) asserting every hard-valid compile is accepted by
  the real `RecipeValidator` — 10M+ runs clean; the fuzz suite's README was reframed as the repo's
  property-fuzz home (untrusted-input + internal-correctness families) as part of this. Unblocks
  **C5** (editor/preview), **B6**, **B9**.*
- [x] **B3** [B] — fan-`Auto` resolver at recipe-compile time + pre-calibration
  heuristic fallback. deps: B2. (§5) *Shipped with B1 as `lib/app_logic/fan_resolver.h`:
  `resolveFans()` turns each phase's tri-state `FanMode` into the resolved on/off the compiled
  Recipe carries (no `Auto` crosses the wire, §9). Explicit On/Off pass through; `Auto` consults the
  fan-conditioned envelopes — conv on when the fan-off heat envelope can't meet the requested ramp,
  cool on when passive cooling is too slow, faster-envelope wins for ASAP — and before calibration
  falls back to the §5 heuristic (conv on while heating, cool on while cooling), flagged so the
  preview labels it estimated. The exact rate/target margins stay §10-open (`kRampMarginFrac`
  placeholder). Host-tested (`test_fan_resolver`).*
- [x] **B4** [B] — `ProfileStore` over a storage port; per-mode dirs;
  stock-vs-user semantics. deps: storage port. (§7, §23)
  **RE-HOMED 2026-07-17 (Wave R2):** the store + port move CYD → controller
  (`lib/control_logic`), storing `oven_Profile`; the CYD becomes a remote client (R3).
  The store logic/tests below are reused near-verbatim, just on the other board.
  *New keyed profile-storage port `IProfileStorage` (`lib/storage_port`) — the multi-entry sibling
  of B5's single-blob `ISettingsStorage` (list/read/write/remove a blob **by name**; the store owns
  the byte layout, the adapter stays dumb) — plus `FakeProfileStorage` (`test/helpers`) and the
  `LittleFsProfileStorage` firmware adapter (`src_cyd/`, one instance per mode dir, the repo's first
  LittleFS user). Host-tested `ProfileStore` (`lib/app_logic`), one instance per mode, mirrors
  `SettingsStore` structurally: it is the **first serialization of the `Phase[]` domain model** as a
  versioned `PersistedBlob` (magic `"PRO1"`, `name` + `mode` + `Phase[]`), with `list`/`load`/`save`/
  `delete`/`duplicate`. Enforces the two decisions design.md leaves to the store: the §23 **stock
  read-only rule** (stock-ness is a **blob flag**, not a directory split — matching §7's single-dir
  phrasing — and save-over/delete of a stock entry are refused, `duplicate` clears the flag) and the
  §7 **never-mixed guard at the store** (a blob whose `mode` byte isn't this store's — a cross-mode
  WiFi upload landing in the wrong dir — is ignored by list/load, not just kept apart by the
  directory). The deserializer is **hardened against untrusted input** (a profile can be pushed over
  serial/WiFi without a reflash, §7): a malformed/short/bad-magic/wrong-version blob is rejected
  never mis-parsed, `phaseCount` is bounded to `kMaxPhases`, the name is always NUL-terminated, and
  `validName` bars path separators / control bytes / `.`/`..` / over-length before any name reaches
  an adapter. **`kMaxPhases` moved `recipe_compiler.h` → `phase.h`** (the Phase-domain fact, so the
  persistence layer needn't pull in the compiler/protobuf; the compiler now `static_assert`s it
  against `kMaxSegments`). `main.cpp` mounts LittleFS at boot and instantiates the two per-mode
  stores as live production wiring (a boot log exercises the whole store→adapter→LittleFS stack);
  the library/editor **screens that drive them are C4/C5**. Host-tested (`test_profile_store`,
  11 cases) **plus a `fuzz/fuzz_profile_store.cpp` harness** (untrusted-blob robustness + a
  save→load round-trip + a "a loaded profile is always a valid `compileRecipe` input" differential
  tying into `fuzz_compiler`) — 11M+ runs clean. **Deferred** (noted in §7/§23, not built here): the
  `data/`-JSON→`uploadfs` stock-seed population + the §24 "Restore stock profiles" action (need a
  seed source — land with C4 / deployment) and recently-used sort ordering (needs a usage clock the
  store doesn't own — `list()` returns a deterministic alphabetical base and C4's ViewModel
  re-sorts). Unblocks **C4** (profile library list/detail) → **C6** (Setup loads a profile as a
  template).
- [x] **B5** [B] — `SettingsStore` + per-field `{min, max, step, units}` config +
  boot-time clamp-to-hard-max. deps: storage port (share B4's). (§4, §24)
  **RE-HOMED 2026-07-17 (Wave R2):** the store + port move CYD → controller; the CYD
  reads/writes via a `SettingsClient` over the link (R3). Bonus: the per-mode caps now
  live on the safety MCU, resolving the §4 defense-in-depth open.
  *`ISettingsStorage` port (`lib/storage_port`) + host-tested `SettingsStore` (`lib/app_logic`):
  versioned blob, load-time re-clamp to current bounds, `NumericFieldConfig`-driven validation
  shared with the numeric editors. NVS/Preferences adapter is thin `src_cyd/` glue. Shipped the
  C8 Settings hub/panels slice (`settings_screen.*`) alongside.*
- [ ] **B8·1** [B] — SD logging PR1: length-delimited protobuf writer + header
  record. deps: SD port. (§7)

## Wave 3 — Bench safety proof, screens, generators (compose Wave 2)

- [x] **A8** [A] — bench `Esp32*` adapters + dummy-load firmware: heartbeat-pull
  test with an LED "heater" shuts off within the timeout. **Closes §8 step 1.**
  deps: A4a, two dev boards (no oven). *A5/A6/A7 enrich a real bench run but aren't
  needed for the fail-safe proof itself.*
  *Proven on two dev boards, no oven: **the heater output dies 745–756 ms after the heartbeats
  stop**, against `kCommandTimeoutMs` = 750 (measured over 4 trials via an edge-triggered bench
  log; the 1 Hz trace is far too coarse to tell a real timeout from a lucky guess). New
  `src_control/` adapters — `Esp32SerialTransport`, `Esp32HeaterSwitch` (GPIO25), `Esp32Contactor`
  (GPIO26), `Esp32Watchdog`, `Esp32Clock` promoted out of `main.cpp` — injected into the A1–A4a
  logic **unchanged**: both `main.cpp`s are recognizable mirrors of `test_reliability_integration`'s
  `Rig`, so `main()` really is the only divergence from the host tests (§11). Both firmwares now
  pump `FrameLink::poll()`/`tick()` from their own loop (neither facade's `service()` does), on a
  fixed 10 ms cadence because TinyFrame's resync timeout counts **`tick()` calls, not ms**.
  **Decision: a bench-only `CONTROL_BENCH` build moves the controller's link to UART2** — §2 pins
  production to UART0 for §25's ROM loader, but that is also the devkit's USB-bridge port, and a
  powered bridge idles TX *driven high*, so it wins arbitration outright (a series resistor only
  picks the loser). The flag therefore also decides whether a **console exists at all**: production
  has none, since the link owns `Serial` — `TF_Error`'s printf is compiled out (`TF_ERROR_QUIET`)
  for the same reason, and both are verified by grepping the built `.elf` rather than trusted.
  **Bench duty stub** (`authorized ? 0.5 : 0`): `SafetySupervisor` only ever cuts and A5's PID does
  not exist, so nothing would raise duty and the LED could never light; at `windowMs=1000` that is a
  500 ms blink = "authorized **and** the loop is alive", while the contactor LED (driven by the
  supervisor itself) is the true `authorized()` readout. `Esp32SerialTransport::write` deliberately
  does **not** clamp to `availableForWrite()`: `TF_WriteImpl` discards the returned count, so a
  short write truncates a frame with no resume path — non-blocking comes from sizing the TX ring
  above `TF_SENDBUF_LEN` instead. CYD link plumbing landed **unconditionally** (it is production
  wiring, and a `session=0`/`enable=false` sender authorizes nothing), with only the boot-time
  Recipe+Start+`HEAT_EN` stimulus behind `esp32dev_cyd_bench`; §19/C6 owns starting a run.
  **Three bugs the bench found that the host tests could not**, all fixed here with regression
  coverage: (1) **`SetupResponder` dedups on seq alone while `ReliableSender` restarts seq at 0
  each boot**, so a rebooted CYD's `Start{seq=1}` was read as a replay — Acked, but
  `onStartAccepted` never fired and the session was silently never adopted; fixed with
  `ReliableSender::setSeqBase()`, seeded from `esp_random()` per boot. (2) **`Handshake` never
  answered a `Hello`**, and *hearing* a peer is not *being heard* — whoever heard first went quiet
  without confirming, so the CYD (~3 s slower to boot) sat at `sawPeer=0` while the controller read
  `matched=1`, and a lone controller reset left it `matched=0` forever. That defeated §9's *decided*
  re-sync and would have made **every watchdog reset take the link down permanently**. Fixed by
  appending a per-boot `boot_nonce` to `Hello` (append-only, so the frozen contract holds) and
  answering a received Hello — immediately on a new/changed nonce, else at most once per
  `kHelloRetryMs`; the rate limit is what makes it terminate, since an answer is never "new" to a
  peer that already knows us. The host tests hid this by having both facades `begin()` at the same
  instant. (3) design.md §11 said `SafetySupervisor.tick()` runs **first** each loop — wrong on the
  merits, since the PID's later `setDuty()` would overwrite `forceOff()` and re-latch a nonzero
  window, losing safety to the PID for a full second; corrected to **last**. New `test_bench_link`
  suite composes `test_reliability_integration`'s two-facade rig with A4a's output stack — the
  first coverage of "real frames stop → the **output** actually cuts" (`test_safety_supervisor`
  reaches into `gate()` directly; `test_reliability_integration` owns no outputs). **§8 step 1's
  three clauses all close:** outputs-default-OFF (controller alone holds `safe=1` indefinitely),
  command-timeout (above), and **watchdog** — a bench hang trigger proves the reset really fires
  and the next boot reports `ResetCause::Watchdog`, which also confirmed that subscribing the task
  by hand (rather than Arduino's `enableLoopWDT()`, whose wrapper would auto-feed the dog) is what
  makes the kick mean anything. **Not in scope:** `Fault{WATCHDOG}` *emission* stays A4b's (A4a
  deferred all FaultCode plumbing) — A8 only logs the cause on the bench. No UV/fan/motor ports
  (A6 owns the logic that would drive them). No PID (A5) — hence the duty stub. **A8 proves the
  cut, not a latch:** restoring the wire re-authorizes on its own, which is correct — A4b's
  `trip()` is what makes a fault sticky. The bench pinout is not production's, so link-on-UART0
  must be re-verified before §25; §8 step 2's re-run against the real chain is where that lands.*
- [x] **A9** [A] — controller→CYD `Telemetry` hot path + CYD-side link health. deps: A2, A8.
  (§9, §14)
  *The half of §9's hot path nobody owned: A2 built the CYD→controller `Heartbeat`, C7 owns
  *consuming* telemetry for the Run screen, and emitting it fell between them — so the link was
  one-way and **the CYD had no way to know the controller existed**. Found the way it had to be:
  the Home indicator kept reading "Link" with the controller physically unplugged. `matched()`
  cannot fill that gap — it answers a different question ("did a peer once answer, and do we
  agree on the `.proto`") and it **latches**. New `protocol::TelemetrySender` mirroring
  `HeartbeatSender` (fire-on-tick, no retransmit — a lost frame self-heals), emitting at §9's
  decided 250 ms **unconditionally, run or no run**, per §9's "unconditionally sends Hello + IDLE
  telemetry". The caller owns the payload via `state()`; the sender only stamps
  session/seq/ctrl_millis and owns the cadence, so it stays ignorant of what telemetry *means* —
  today the controller emits an otherwise-zeroed IDLE frame carrying `heater_duty`, read **after**
  `SafetySupervisor::tick()` so it reports what the outputs actually did rather than what the
  control loop asked for; A5/A6/D4 fill in the rest. `CydLink::linkAlive()` reads *arrival* (not
  contents) as the liveness proof. **New `kLinkTimeoutMs = 1000`** (~4 missed frames — the same
  "miss 3-4 and act" logic as `kCommandTimeoutMs`, in the other direction); deliberately **not**
  the same number as `FaultController::linkTimeoutMs` = 2000, which is a separate, more patient
  run-scoped choice about when to throw a red modal (§22). Both are **TBD §10** placeholders.
  `HomeViewModel::linkStateFrom` gained an `alive` input and consults it **first**, since it is
  the only one that decays; `saw_peer`/`matched` still distinguish "check the cable" from
  "reflash the pair", which must never collapse into one state. **`HeartbeatMonitor` moved
  `lib/control_logic` → `lib/protocol`** (+ `namespace protocol`): both ends ask the identical
  question of the other's hot-path stream — `SessionGate` of `Heartbeat` at `kCommandTimeoutMs`,
  `CydLink` of `Telemetry` at `kLinkTimeoutMs` — so one implementation now serves both directions
  instead of `lib/protocol` reaching up into `control_logic`. B7's `FaultController` still
  deliberately reuses the *pattern* rather than the class (it owns no clock port); its comment was
  updated, not its reasoning. Also added `HeaterActuator::duty()` so telemetry can report the
  post-safety truth. **Unblocks** §22's `FaultController.linkHealthy`, which B7 left as a
  parameter with no producer. Door-open wake (§17) still waits on D1/D3 — the transport exists
  now, the sensor doesn't. Verified on the bench: holding the controller in reset reads
  `matched=1 sawPeer=1 alive=0 state=LINK_NONE` — the latch and the decay disagreeing, exactly as
  intended — and it recovers on its own when telemetry resumes.*
- [x] **A10** [A] — physical-oven plant simulator for bench testing with the real
  controller. A thermal-plant model — heater duty → wall/workpiece temperature via the
  shared first-order envelopes + lag (reuse `thermal_math.h` / `oven_cal.h` so the sim and
  the planner/preview agree), passive cooling (no chamber cool fan, §6), plus convection-fan
  and UV/turntable effects — that consumes the controller's output commands and feeds back
  **synthetic thermocouple readings**, closing the control loop with no oven. Lets the full
  run path be exercised end-to-end against the real controller firmware on the two-devkit
  bench (A8) before mains hardware (D3/D4) exists: the executor's segment sequencing +
  `RAMP_ASAP`/hold-entry gating (A6), the PID + feedforward (A5), the safety supervisor's
  clamps/trips (A4a/A4b), and the implicit **backup cooldown to touch-safe** (§5/§6) — e.g.
  drive a reflow profile and watch it ramp, soak, peak, then coast to 43 °C and report DONE.
  Form to decide: a controller-side fake `IThermocouples` adapter that runs the plant model
  on-device from the commanded outputs (loop closes entirely on the controller, host reads
  telemetry), and/or a host/second-devkit plant node — pick during design; keep the model in
  `lib/` (host-testable, board-agnostic) with only the injection adapter board-specific.
  deps: A5, A6, A8. (§5, §6, §8 step 1)
  **DONE 2026-07-18.** Form chosen (with the user): a **physics twin + back-fit cal**
  (first-principles lumped-capacitance model, then linearized into `oven_cal.h` so the sim and
  the CYD preview/ETA agree by construction), running **on-device + host** (real firmware via
  `CONTROL_SIM`, plus a host closed-loop harness), with **physics-anchored** constants (the donor
  is assumed faulty, so no measured heat-up data). Landed: `lib/plant/oven_plant.h` (energy model —
  loss-limited ceiling, asymptotic cooling, element overshoot) + `lib/plant/sim_thermocouples.h`
  (an `IThermocouples` backed by the plant, with quantization + open/short fault injection);
  `lib/control_logic/run_path.h` — the first composition of executor + PID + safety + heater into
  one tick sequence, which D4 reuses (and which surfaced a real bug: an executor fault wasn't
  reaching the supervisor); `oven_cal.h` re-authored physics-anchored (`CALIBRATED` stays false);
  `esp32dev_control_sim` env; `test_oven_plant` + a cal-consistency test, the closed-loop
  `test_sim_run`, and `fuzz_oven_plant` / `fuzz_sim_run` (plant robustness + whole-run
  safety/liveness/cooldown invariants). Bench-verified end-to-end on the two-devkit rig: a cure
  profile ramped → held → coasted to 43 °C → reported **DONE** (fault=0). Follow-on the same day:
  the CYD Home screen was wired to the sim's telemetry (temp-driven status badge + readout colours,
  sleep gating), and the touch-safe temperature was unified into one shared
  `lib/calibration/touch_safe.h` (`oven_domain::kTouchSafeC`).
- [x] **C4** [C] — profile library (list + detail). deps: B4, C3. (§23)
  **REWIRED 2026-07-17 (Wave R3):** the view-model binds to a `ProfileClient` (remote,
  over the link) instead of a local `ProfileStore`, gaining loading/error states; the
  curve/facts math stays CYD-side, fed by the fetched `Profile`.
  *Shipped as a self-contained hub-and-spoke controller (the `SettingsScreen` pattern): Home →
  Profiles opens a **Cure|Reflow chooser** — **two big Home-style tiles** ("UV CURE PROFILES" /
  "REFLOW PROFILES"), a direct tap rather than a menu since there are only two profile types (the
  entry is mode-blind; §23's "never mixed" is enforced by picking a mode first) — then the fixed-mode
  **list** → profile **detail/actions**. New
  `profile_facts.h` (pure `lib/app_logic`) is the shared derived-fact layer — peak + estimated
  duration (∫dT/rate + holds) for the row/detail facts, and the **requested/achievable curve point
  sampling** — computed off a loaded profile's phases via the §12 curve math (`thermal_math`), no
  `OvenModel` reach-in. New `profile_curve` widget (`lib/ui_logic`) draws those two polylines in a
  themed instrument box (requested = dim dashed, achievable = accent solid, + the "uncalibrated"
  note) — the **minimal first cut of C5's feasibility widget** (per the decision to draw a real curve
  now); C5 extends the *same* widget to amber-divergence flags + closed-loop overshoot, reusing the
  point math. `ProfileLibraryViewModel` (per mode, pure) supplies rows/facts and the store-mutating
  actions: **Dup** (owns the `copy` / `copy 2` … suffix naming + collision resolution the store left
  to the UI) and
  **Delete** (behind a new reusable `confirm_dialog` — a *simple* confirm, **not** the press-and-hold
  arm gesture §19/§22 reserve for energizing hazards, and **not** danger-red for the same reason).
  §23 **stock gating** rendered: Edit → "Save as", Delete disabled. `selectable_list` gained an
  optional **leading footer action** (the `+ New` button; the settings hub passes none) and
  `kMaxItems` 12→32 to hold a full library (a `static_assert` in the screen ties it to
  `ProfileStore::kMaxListed`); its **Open button disables when no row is selectable** (`canOpen()`),
  so an empty library shows Open greyed rather than falsely pressable. **New/Edit/Load** leave for
  screens C4 does not own (editor §12/C5, Setup §19/C6) — published as `NAV_PROFILE_*` intents,
  observed only by tests until those land (the posture Home's tile intents held before Settings
  existed; the *which-profile* + working-copy handoff is a seam C5/C6 own). Wired into `main.cpp`
  (router `SCREEN_PROFILES`, **cached like Home** with a reset-to-chooser hook — the stateless
  two-tile chooser is its default view; the list/detail pages rebuild on demand and re-read the store,
  so caching the resident screen is safe) over the two `ProfileStore`s already instantiated at boot;
  the `·` middot separating the row facts was added to `red_hat_mono_14/16` (the pipeline reproduced
  the committed 14px glyphs byte-for-byte first).
  Host-tested `test_profile_library` (native_ui_cyd, both geometries — driven through the real button
  seams so it is geometry-independent) + `test_profile_facts` (native_logic) **plus
  `fuzz/fuzz_profile_facts.cpp`** (profile_facts is a new consumer of an untrusted loaded `Phase[]`,
  §7 — facts/curve stay finite + bounded, no NaN reaches `lv_line`; 3.7M+ runs clean).
  Two incidental fixes rode along: **`oven_cal::DEFAULT` → `kDefaultModel`** (C4 is the first
  CYD-firmware consumer of `oven_cal.h`, and `DEFAULT` collided with Arduino's `DEFAULT` macro — a
  latent landmine for any firmware TU including both), and **`board_build.filesystem = littlefs`** on
  the CYD envs so `uploadfs` builds a LittleFS image matching the firmware's mount (the PlatformIO
  default is SPIFFS, which would not mount). **Verified end-to-end on the 3.5" board** (flash +
  `uploadfs` demo fixtures `data/profiles/` → boot reports `[profiles] cure=1 reflow=2`; chooser →
  list → detail curve render on glass, stock gating on the seeded stock profile).
  **Deferred**: the **recently-used sort** (no usage clock — `list()` stays alphabetical), and the
  **stock-seed *feature*** — a reviewable seed source (JSON→generator), §24 "Restore stock profiles",
  and the marking semantics (needs a seed-source decision; the `uploadfs` mechanism + demo fixtures
  above are in place, but the feature itself is future work) — a fresh flash otherwise shows the §23
  empty state. **Pick context** (Setup → Load) entry lands with C6; the manage-context CRUD is
  complete. Unblocks **C6**.*
- [x] **C5** [C] — profile editor (2 PRs: overview + phase-editor field list;
  then feasibility-curve preview). deps: C1, B1, B2. (§12)
  **REWIRED 2026-07-17 (Wave R3):** still edits an in-RAM `Phase[]` working copy (preview
  unchanged), but opening fetches via `ProfileGetReq` and Save issues `ProfilePut` over
  the link (a `phase_codec` at the wire boundary), gaining a saving/save-failed state.
  *Shipped as `ProfileEditorScreen` (`lib/ui_logic/profile_editor_screen.*`), the `SettingsScreen`
  hub-and-spoke controller cloned: **Overview** (feasibility curve + one-phase-per-row list + Save)
  → **Phase editor** (a field list per phase) → the shared numeric keypad → **name entry**. Edits
  parameters, never the curve (§12): each phase number routes through `NumericFieldConfig` +
  `create_numeric_keypad` (all wide-range → keypad per the >20-step rule), fans cycle Auto→On→Off
  in place, UV/motor toggle (cure only), and the cure **Hold** field's label/amber note track the
  exposure-vs-raw-seconds semantics. Works on a **working copy** via `beginEdit(profile, store,
  saveAs)`; only Save commits (a stock source or a fresh New routes through name entry, §23 Save-as);
  Save is gated on `compileRecipe().hardValid` (red word blocks) with an amber word for a
  physically-optimistic profile (`hasAmber()`) — the shared validator, never a second one. The
  **Advanced** path (add/delete/reorder phases) is gated on `subj_advanced`. New
  `profile_templates.h` seeds NEW from the fixed per-mode templates (reflow preheat/soak/reflow/cool,
  cure warm/cure/cool) + seeds each phase's name from its role label (`seedPhaseName`; see the
  per-phase-names follow-up below).
  **PR2 feasibility preview** extends `profile_facts` + the C4 `profile_curve` widget (the same one,
  as C4 flagged): `sampleCurve`/`computeFacts` now **fan-Auto-resolve** the achievable curve via
  `fan_resolver` (so preview and compiled advisories agree on fans); new `anyRampRateLimited` drives
  an **amber divergence flag** (the requested line goes amber where the oven can't meet it); new
  `sampleOvershoot` runs the achievable trajectory through the `{a,b,τ}` lag as a faint **closed-loop
  settling** trace (§12's optional-later, bounded to `kMaxCurvePoints` steps). Wired into `main.cpp`
  (`SCREEN_EDITOR`; the `NAV_PROFILE_NEW/EDIT` seams C4 reserved are resolved in the composition root
  off the library's selection; exit returns to the edited mode's list). **The editor is
  heap-allocated on first use, not a static**: the library's two view-models already fill the static
  DRAM segment and the editor is just as large, but the two are never co-visible — a static both
  overflowed `dram0_0_seg` by 3.3 kB. Two build-config facts: the **UI + sim lanes gained nanopb**
  (the editor validates via the shared `recipe_compiler` → generated `oven.pb.h`; C8's fault overlay
  will need it too) via a `lib/ui_logic/library.json` (the `lib/protocol` pattern) + `native_ui_cyd`/
  `sim_common` deps. Host-tested `test_profile_editor` (both geometries, driven through the button
  seams) + `test_profile_templates` + extended `test_profile_facts`; `fuzz_profile_facts` extended to
  the iterative overshoot + divergence math (5.2 M+ runs clean). Verified in the sim on the 3.5"
  (Overview curve with all three traces + amber divergence; Phase editor field list with fan
  resolution). Unblocks **C6** (Setup loads a library profile as a run template into this editor).*
  *Follow-up — per-phase names + name-entry keyboard: the `Phase` domain model gained a stored
  `name` (`kPhaseNameCap=16`, `phase.h`), seeded from the template role and editable from a new
  **Name** row in the phase editor; `phaseLabel()` collapsed to a seed-time helper and every display
  site reads `phase.name`. Persisted in the `ProfileStore` blob — **no version bump** (pre-release);
  the demo `data/profiles/**/*.bin` fixtures were regenerated by a new host generator
  `tools/gen_profiles.cpp`, and each phase name is NUL-terminated on the untrusted load path
  (`copyOut`). The profile name (Save-as) and phase rename share **one** on-screen keyboard
  (`buildNameEntry`, targeted by `naming_phase_`): a compact custom LVGL map tuned for short names
  (letters + `⌫` + `✓`; mode `abc/ABC/1#`; the header **Back** is the only cancel — no on-keyboard
  ✗), height-bounded + bottom-pinned, with per-key **popover previews** (`lv_keyboard_set_popovers`)
  and no auto-repeat. Added `⌫` (0xF55A) to the body fonts `red_hat_mono_14/16` (the default
  keyboard's newline ↵ 0xF8A2 isn't in Font Awesome free — hence the custom map), and bumped
  `LV_MEM_SIZE` 64→80 kB for the popover's compositing layer. **[Corrected 2026-07-17: that
  parenthetical was wrong — `LV_MEM_SIZE` is a STATIC pool (`work_mem_int` in `dram0_0_seg`), not
  runtime heap, and 80 kB overflows a clean build by ~15.7 kB; it only ever linked because
  incremental builds reused a stale pool object (an `lv_conf.h` edit doesn't retrigger the LVGL
  recompile). Reverted to 64 kB. See the DRAM-budget note in design.md "Consequences worth
  knowing".]** Host-tested (`test_profile_store` name round-trip + untrusted NUL-term,
  `test_profile_templates` seeded names, `test_profile_editor` rename + Back-cancel seams at both
  geometries); verified in the sim and on the 3.5" hardware.*
- [ ] **C7** [C] — Run/Monitor (3 PRs: layout/telemetry/STOP; projected-vs-actual
  chart + live ETA; cure paused/resume overlay). deps: C3, B2; soft: B6 (resume
  overlay, 3rd PR). (§15)
- [ ] **Bench-found follow-ups** (C6/C7 validation on the two-devkit bench against the A10 sim,
  2026-07-19; a full cure ran ramp→hold→cool→"Run complete" end-to-end):
  - **Ramp overshoot mitigation (control loop, §5).** On a fast ASAP ramp into a hold the PID
    holds full duty through the whole ramp and overcharges the calrod (elementC≈1000 J/K); when the
    control temp reaches setpoint the stored element heat carries the chamber ~15 °C past it (a
    60 °C cure peaked ~75 °C on the sim). It settles, but the overshoot (a) trips the §16 deviation
    cue and (b) stretches the cool-down. Fix is control-side (A5/A6): feedforward/derivative that
    eases duty off *before* setpoint so the element isn't overcharged. A real oven with this
    element mass would do the same — a genuine control-quality item, not sim-only. (deps: D7 PID
    tuning.)
  - **Door-open dismisses "Run complete" — but NOT a fault (§15/§16/§22).** Opening the oven door
    on the terminal "Run complete" page should clear it to Home (the operator has opened the door
    to retrieve the workpiece — the run is over and acknowledged by the act). A "Fault - run ended"
    page must NOT be door-dismissable: a fault demands an explicit acknowledge (§22). Needs the
    controller's `door_open` telemetry wired to the Run screen's Ended page. (goes with C8's fault
    overlay + acknowledge path.)
- [ ] **C8** [C] — Run Summary (§16) + Fault overlay (§22) + Settings hub + panels
  (§24), one each. deps: B7, B5, C1, C2, C3. *All deps now landed. Settings hub + panels slice
  already shipped with B5 (`settings_screen.*`); Run Summary + Fault overlay remain. B7 shipped
  the logic both bind to: the Fault overlay renders `fault_table::faultInfo/formatTitle` and
  drives `FaultController` (`state()`/`updatedAtMs` to diff into `lv_subject_t`, `acknowledge()`
  → `AckRoute`, `active()` for the RGB-LED/buzzer, `overTempLatched()` → §14 HOT / §17 sleep);
  Run Summary renders `RunFitResult` + `advisoryText()` as an inline amber banner (never the
  modal, §22). Still C8's: the buzzer pattern/volume (TBD §10) and a review pass on B7's draft
  advisory strings.*
- [x] **C10** [C] — second CYD board variant (3.5" ST7796S 320×480 portrait) + the board
  abstraction that makes a third cheap. deps: none (HMI-side only). *Motivated by §21, not by
  the extra pixels: the 3.5" board **survives WiFi bring-up** (1 boot, 0 brownouts, radio up,
  link `matched=1`) where the 2.8"'s AMS1117 collapses — and without a radio there is no OTA,
  the controller's only field reflash path (§25). It is now `default_envs`; the 2.8" stays
  supported and both are built by `make build` + CI so neither rots.*
  *The rule that made it cheap: **nothing under `lib/` learns a board identity.** `CYD_BOARD_*`
  is read only by the new `include/cyd_board.h` (pins, orientation, buffers, capabilities);
  `lib/` sees neutral geometry (`lib/panel/panel.h` → `panel::kPortrait`; `theme.h` re-authored
  in **millimetres**, `static_assert`ing that the 2.8"'s numbers are byte-identical) and
  capabilities as **data** (`subj_has_ambient_light`, `device_info.h`). Fonts are the one thing
  mm cannot scale, so 16/32 px are picked by **pitch** — generation verified reproducible
  (re-running the recipe reproduces the committed 14 px font byte-for-byte). Ports finally
  earned their keep: `LgfxDisplay`/`LgfxTouch`/`InjectedTouch` (§11's claim was aspirational
  until now). Full inventory + consequences in §6a.*
  *Five latent bugs it flushed out, all pre-existing: an unsigned-underflow that expired the
  §17 sleep timeout instantly (latent on the 2.8", fatal on the slower-flushing 3.5");
  `pio test -e embedded` hadn't compiled in months (now in `make build` + CI); `dev-shot`
  silently wrote black PNGs on an unreadable panel (now 501); LVGL's default theme was tinting
  our dark UI's DISABLED state **lighter** via `recolor` (the honest cause of "washed out");
  and ✕ Cancel slid 38 px out from under the finger as you typed (`flex_grow` on a hidden
  child). Also: a `pinMode`-after-`digitalWrite` on the controller's contactor (§4 fail-safe
  adjacent) — see the fix in `Esp32Contactor`.*
  *Open: the 3.5" panel's contrast is poor (blacks read grey at every backlight level), which
  is a real input to §23's ISA-101 palette — flagged as an open in §6a/§10. And
  `PANEL_PX_PER_MM_X100=649` is nominal; measure the active area with calipers (654 would move
  `STEPPER_BTN` 111→112).*
- [ ] **B6** [B] — remainder-profile generator for cure resume. deps: B1. (§15)
- [ ] **B9** [B] — random-profile generator within safety bounds. deps: B1, B2. (§5, §20)
- [ ] **C6** [C] — Setup + Confirm. deps: C4, C5 (loads a library profile as a
  template into the editor). *Do after C4/C5.* (§19)

## Wave 4 — Mains hardware + complete safety chain (§8 step 2; gated by D1)

- [ ] **D2** [D] — interlock/mains one-line diagram. deps: D1. *Required
  deliverable before any mains work.* (§6)
- [ ] **D3** [D] — safety-chain fitout: donor interlock chain, cutoffs, fuse,
  contactor, high-limit, SSR, UV door switch, window film — then re-run A8's
  step-1 fail-safe proof against the real chain. deps: D1, D2, A8. (§4, §8 step 2)
- [ ] **D4** [D] — TC front-end IC selection + wall-TC channel bring-up, then
  workpiece-TC install (pass-through, panel jack, reflow rack) as separate items.
  *Provides the real temp-input adapter.* deps: D1, D3. (§6)

## Wave 5 — First vertical slice: UV cure end-to-end (§8 step 3)

- [ ] **D5** [D] — UV cure end-to-end. The first full vertical slice; mostly
  integration once the software exists. deps: A4–A7, B1–B4, C3–C7, D3, D4.
  (§8 step 3)
- [ ] **B8·2** [B] — SD logging PR2: RAM ring buffer + dedicated link task
  isolating heartbeat TX from card stalls. deps: B8·1, hardware (to measure real
  stalls). (§7)
- [ ] **D6·tools** [D] — PC-side tools: log decoder (protobuf → DuckDB/CSV/Parquet) +
  fit → `oven_cal.h` emitter. deps: B8·1 log format. *Hardware-independent — can
  start as soon as the format lands.* (§6, §7)

## Wave 6 — Reflow, PCB, connectivity (§8 steps 4–6)

- [ ] **D6·wizard** [D] — calibration-wizard screens. deps: D4, D5. (§20)
- [ ] **D7** [D] — reflow bring-up: PID tuning, calibration runs, drift
  thresholds. deps: D5, D4, D6. (§8 step 4)
- [ ] **D8** [D] — custom PCB (same ESP32 target); move firmware over unchanged,
  re-verify safety on real hardware. deps: D7. (§8 step 5)
- [ ] **D9** [D] — WiFi/OTA **on the controller** (several PRs; REVISED 2026-07-17 per
  the §2 CYD-is-a-remote change, see Wave R below): WiFi + HTTP data server **on the
  controller's radio**; controller **self-OTA over WiFi** (standard A/B — the old
  CYD-as-ISP / ESP32 ROM-loader client / GPIO0-EN leg is **dropped**); `OtaController`
  state machine (host-testable with fakes); OTA wizard screens (CYD renders, controller
  acts); controller partition-fit (app twice + LittleFS in 4 MB) measurement.
  **New opens (§10):** the **CYD's own** field-reflash path once WiFi leaves it (USB vs
  controller-as-ISP), and the **data-server↔CYD-SD bridge** (logs are on the CYD's SD).
  deps: D6·tools pipeline, hardware, Wave R. (§21, §25, §27; §8 step 6)

## Wave R — CYD-is-a-UI-remote re-architecture (2026-07-17; storage + protocol)

The §2 revision: the CYD ran out of static DRAM to host the profile/settings stores +
WiFi alongside LVGL (§6a), so those responsibilities move to the controller and the CYD
becomes a remote client. This wave lands the **storage + protocol** move (the memory win
→ bigger render buffers); WiFi/OTA/data-server stay design-only (D9). Details →
design.md §2/§7/§9/§11/§21/§25/§27. Ordered; each ~one PR.

- [x] **R1** [A] — **protocol expansion.** Add typed `Phase`/`Profile`/`ProfileSummary`/
  `Settings` + management messages (`ProfileList/Get/Put/Delete/Dup/Rename`,
  `SettingsGet/Put`) to `proto/oven.proto` + `oven.options`; new `kTfType*` ids
  (`0x19+`) + router cases + `IMessageObserver` virtuals; the new **request/reply**
  reliability pair (`RequestClient`/`RequestResponder`, request→data-reply) in
  `lib/protocol`. Host round-trip tests (`native_control`). *Schema hash changes —
  expected (matched-pair reflash).* deps: none. (§9)
- [x] **R2** [A/B] — **controller owns the stores.** Relocate `IProfileStorage`/
  `ISettingsStorage`; move `ProfileStore`/`SettingsStore` → `lib/control_logic` (store
  `oven_Profile`); `LittleFsProfileStorage`/settings adapter in `src_control`; controller
  partition + `board_build.filesystem=littlefs` + `uploadfs` stock seeds via updated
  `tools/gen_profiles.cpp`; `ProfileResponder`/`SettingsResponder` into `ControllerLink`.
  Move `fuzz_profile_store` to the control context; add `fuzz_profile_wire` (controller
  parsing untrusted management frames — it is the safety MCU). deps: R1. (§7/§11/§23)
- [x] **R3** [B/C] — **CYD remote client.** `phase_codec` (`Phase↔oven_Phase`);
  `ProfileClient`/`SettingsClient` (`lib/app_logic`) over the request/reply path; rewire
  `ProfileLibraryViewModel`, `ProfileEditorScreen`, `SettingsScreen` to async with
  **loading / error / saving** states. Drop the CYD's LittleFS/NVS/`ProfileStore`/
  `SettingsStore` + mount + `data/` + `filesystem=littlefs`. `native_logic_cyd` +
  `native_ui_cyd`(+`_35`) tests against a fake client. deps: R1, R2. (§12/§23/§24)
- [x] **R4** [C] — **reclaim the memory (the payoff).** With the stores + WiFi gone from
  production, raise `DRAW_BUF_LINES` (`include/cyd_board.h`, toward 48–60 per §6a's
  measured menu) and optionally restore the 80 kB LVGL pool — each **clean-build verified**
  (`pio run -t clean`). Re-measure with `make perf` + the on-glass perf probe. deps: R3.
  (§6a)
