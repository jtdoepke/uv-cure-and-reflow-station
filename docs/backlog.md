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
- [x] **A3** [A] — `HeaterActuator` time-proportioning class. (§11)

## New ports to create (not standalone items — build each with its first consumer)

Several waves depend on ports that don't exist yet. Each is one pure interface in
`lib/control_port/` (or a CYD storage port) + a `test/helpers` fake, matching the
existing `IClock`/`IHeaterSwitch` idiom:

- **Temp-input** (control-sensor readback; §11 `IThermocouples`) — first needed by **A5/A6**, also **A4b**.
- **`IContactor`** (energize-to-close; §4) — **A4a**. *Landed with A4a: `lib/control_port/IContactor.h`
  plus `FakeContactor` (`test/helpers`).*
- **`IWatchdog`** (kick + reset-cause readback; §9/§11) — **A4b**, **A8**.
- **Output ports** `IUvOutput` / `IFanOutput` / `IMotorOutput` (§11) — **A6** actuation wiring / **A8** / **D5**.
- **Storage port** (LittleFS adapter + in-memory fake; §7) — **B4**, shared by **B5**.
  *Partially landed: **B5** shipped `ISettingsStorage` + a `FakeSettingsStorage` (`lib/storage_port`);
  **B4** still needs the profile-blob/LittleFS storage port.*
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
- [x] **C9** [C] — sleep/wake (§17) + auto-brightness (§18). deps: none. *Ports
  `IAmbientLight`/`IBacklight` (`lib/display_port`) + host-tested `AutoBrightness`
  (filter→curve→additive-bias→hysteresis→ramp→floor/ceiling; sole backlight owner) and
  `SleepController` (idle-only, wake-tap consumed, never sleep non-idle) in `lib/app_logic`;
  ESP32 adapters + `main.cpp` wiring with live bias preview. Curve calibrated + inverted for
  this board's LDR (GPIO34 reads ~0 in room light, climbs in the dark). Verified on hardware.
  Door-open wake deferred until the controller reports door state over telemetry (§9).*
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
- [ ] **A4b** [A] — `SafetySupervisor` L3: setpoint clamp, per-mode over-temp trip,
  stuck-heater plausibility, bounded total runtime, reset-cause → `Fault{WATCHDOG}`.
  deps: A4a, temp-input port, `IWatchdog`; soft: B2 (runtime bound uses `oven_cal`). (§4)
- [ ] **A5** [A] — PID + anti-windup + feedforward hook, tested against a toy
  first-order plant. deps: A3, temp-input port; soft: B2 (feedforward constants). (§5)
- [ ] **A6** [A] — profile executor: segment sequencing, `RAMP_ASAP` target
  gating, hold-entry gate, per-segment watchdog. deps: temp-input port. *Emits the
  setpoint A5's PID tracks — buildable/testable independently of A5.* (§5)
- [ ] **B1** [B] — phase→segment compiler: {target, ramp `x`, hold `y`} → generic
  segments; two-tier validation; exposure→hold-seconds math with fallback/labeling.
  deps: B2. (§5, §12)
- [ ] **B3** [B] — fan-`Auto` resolver at recipe-compile time + pre-calibration
  heuristic fallback. deps: B2. (§5)
- [ ] **B4** [B] — `ProfileStore` over a storage port; per-mode dirs;
  stock-vs-user semantics. deps: storage port. (§7, §23)
- [x] **B5** [B] — `SettingsStore` + per-field `{min, max, step, units}` config +
  boot-time clamp-to-hard-max. deps: storage port (share B4's). (§4, §24)
  *`ISettingsStorage` port (`lib/storage_port`) + host-tested `SettingsStore` (`lib/app_logic`):
  versioned blob, load-time re-clamp to current bounds, `NumericFieldConfig`-driven validation
  shared with the numeric editors. NVS/Preferences adapter is thin `src_cyd/` glue. Shipped the
  C8 Settings hub/panels slice (`settings_screen.*`) alongside.*
- [ ] **B8·1** [B] — SD logging PR1: length-delimited protobuf writer + header
  record. deps: SD port. (§7)

## Wave 3 — Bench safety proof, screens, generators (compose Wave 2)

- [ ] **A8** [A] — bench `Esp32*` adapters + dummy-load firmware: heartbeat-pull
  test with an LED "heater" shuts off within the timeout. **Closes §8 step 1.**
  deps: A4a, two dev boards (no oven). *A5/A6/A7 enrich a real bench run but aren't
  needed for the fail-safe proof itself.*
- [ ] **C4** [C] — profile library (list + detail). deps: B4, C3. (§23)
- [ ] **C5** [C] — profile editor (2 PRs: overview + phase-editor field list;
  then feasibility-curve preview). deps: C1, B1, B2. (§12)
- [ ] **C7** [C] — Run/Monitor (3 PRs: layout/telemetry/STOP; projected-vs-actual
  chart + live ETA; cure paused/resume overlay). deps: C3, B2; soft: B6 (resume
  overlay, 3rd PR). (§15)
- [ ] **C8** [C] — Run Summary (§16) + Fault overlay (§22) + Settings hub + panels
  (§24), one each. deps: B7, B5, C1, C2, C3. *All deps now landed. Settings hub + panels slice
  already shipped with B5 (`settings_screen.*`); Run Summary + Fault overlay remain. B7 shipped
  the logic both bind to: the Fault overlay renders `fault_table::faultInfo/formatTitle` and
  drives `FaultController` (`state()`/`updatedAtMs` to diff into `lv_subject_t`, `acknowledge()`
  → `AckRoute`, `active()` for the RGB-LED/buzzer, `overTempLatched()` → §14 HOT / §17 sleep);
  Run Summary renders `RunFitResult` + `advisoryText()` as an inline amber banner (never the
  modal, §22). Still C8's: the buzzer pattern/volume (TBD §10) and a review pass on B7's draft
  advisory strings.*
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
- [ ] **D9** [D] — WiFi/OTA (several PRs): HTTP data server; bundle format +
  validation; ESP32 ROM-loader client; `OtaController` state machine
  (host-testable with fakes); OTA wizard screens; partition-fit measurement.
  deps: D6·tools pipeline, hardware, GPIO0/EN control lines wired. (§21, §25, §27; §8 step 6)
