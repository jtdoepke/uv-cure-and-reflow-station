# Backlog — work breakdown for docs/design.md

Each item is sized to roughly one PR. Section references (§) point into
`docs/design.md`. The spine is the §8 build sequence, but most items are
host-testable software that doesn't wait for hardware: after Milestone 0,
Tracks A–C run fully in parallel, and hardware Track D only truly blocks
items A8, B8's task-split, and D3–D5/D7.

Open questions (§10) are tagged there with the §8 step they block — resolve
each before starting that step's items. The step-1 blockers are Milestone 0.

## Milestone 0 — Plumbing (do first; everything depends on it)

- [x] **M0.1 — Controller env + skeleton + test lane.** New PlatformIO env for
  the controller; `lib/control_port/` + `lib/control_logic/` skeleton; a
  controller lane in the three-tier native test setup. Empty-but-compiling
  structure with one trivial test per lane. (§3, §10, §11)
- [ ] **M0.2 — nanopb + TinyFrame deps + protobuf codegen step** wired into
  both envs, proven with a dummy message. (§9, §10) *(nanopb + codegen done;
  TinyFrame submodule + framing glue outstanding)*
- [x] **M0.3 — `.proto` v1 + schema-hash build step.** `Hello` (frozen
  append-only bootstrap contract), `Recipe`/`Segment`, `Start`, `Heartbeat`,
  `Abort`, `Telemetry`, `Ack`/`Nak`, `Done`, `Fault` + the `faultCode` enum.
  Most other items code against this; land it early even if fields churn
  behind the schema gate. (§9, §22)

## Track A — Protocol & controller logic (pure software, host-tested)

- [ ] **A1 — TinyFrame framing + `ISerialTransport` port + in-memory pipe**,
  with encode/decode round-trip tests. (§9 code shape)
- [ ] **A2 — Reliability layer:** heartbeat tick, seq/ACK-NAK retry, session
  semantics, schema-hash handshake gate. (§9)
- [ ] **A3 — `HeaterActuator` time-proportioning class.** §11 specs this to
  the point of being a ready-made ticket, including its test plan
  (`FakeClock` + recording `IHeaterSwitch`). (§11)
- [ ] **A4 — `SafetySupervisor`** (2–3 PRs): command-timeout + fail-safe
  defaults + contactor policy first; L3 clamps, runtime bound, per-mode
  over-temp trip, stuck-heater plausibility, reset-cause →
  `Fault{WATCHDOG}` later. (§4, §9)
- [ ] **A5 — PID + anti-windup + feedforward hook**, tested against a toy
  first-order plant model. (§5)
- [ ] **A6 — Profile executor:** segment sequencing, `RAMP_ASAP` target
  gating, hold-entry gate, per-segment watchdog. (§5)
- [ ] **A7 — Controller-side recipe validation:** range checks,
  mode-from-content derivation, NAK reasons. (§4, §9)
- [ ] **A8 — Bench `Esp32*` adapters + dummy-load firmware.** Needs the two
  dev boards but no oven: heartbeat-pull test with an LED "heater" shuts off
  within the timeout. Closes §8 step 1.

## Track B — CYD app logic (pure software)

- [ ] **B1 — Phase→segment compiler:** {target, ramp `x`, hold `y`} → generic
  segments; two-tier validation; exposure→hold-seconds math with its
  fallback/labeling rules. (§5, §12)
- [ ] **B2 — Shared rate-limit/lag math + stub `oven_cal.h`** with default
  constants. One library, several consumers: feasibility preview, ETA,
  fan-`Auto`, controller feedforward. (§6, §12, §15)
- [ ] **B3 — Fan-`Auto` resolver** at recipe-compile time, with the
  pre-calibration heuristic fallback. (§5)
- [ ] **B4 — `ProfileStore`** over a storage port (LittleFS adapter +
  in-memory fake); per-mode dirs; stock-vs-user semantics. (§7, §23)
- [ ] **B5 — `SettingsStore`** + per-field `{min, max, step, units}` config +
  boot-time clamp-to-hard-max. (§4, §24)
- [ ] **B6 — Remainder-profile generator** for cure resume. Small,
  self-contained, host-testable. (§15)
- [ ] **B7 — Fault table + latching/ack-routing logic** (§22) and
  **run-summary residual math** (§16) — one small PR each.
- [ ] **B8 — SD logging** (2 PRs): length-delimited protobuf writer + header
  record first; then the RAM ring buffer + dedicated link task isolating
  heartbeat TX from card stalls (the task split needs hardware to measure
  real stalls). (§7)
- [ ] **B9 — Random-profile generator** within safety bounds. (§5, §20)

## Track C — UI screens (simulator-driven; one screen ≈ one PR)

Ordered by reuse: shared widgets first, then screens in dependency order.

- [ ] **C1 — `NumericEntry` logic + keypad widget.** Everything numeric
  routes through it. (§26)
- [ ] **C2 — Value-stepper widget.** (§24)
- [ ] **C3 — Home/Status hub.** Sets the visual language; do early. (§14)
- [ ] **C4 — Profile library** (list + detail). (§23)
- [ ] **C5 — Profile editor** (2 PRs): overview + phase-editor field list
  first; feasibility curve preview second. (§12)
- [ ] **C6 — Setup + Confirm.** (§19)
- [ ] **C7 — Run/Monitor** (3 PRs): layout/telemetry/STOP; projected-vs-actual
  chart + live ETA; cure paused/resume overlay. (§15)
- [ ] **C8 — Run Summary** (§16), **Fault overlay** (§22), **Settings hub +
  panels** (§24) — one each.
- [ ] **C9 — Sleep/wake** (§17) and **auto-brightness** (§18). Both small and
  self-contained; auto-brightness touches nothing else — a good standalone
  starter.

## Track D — Hardware (the real sequence gate, §8 steps 2–5)

- [ ] **D1 — §10 teardown verifications** (one investigation task): fan motor
  type, cooling-fan existence, SMPS reuse, humidity-sensor interface, relay
  board. Gates the `Recipe` schema details and the drivers. (§6, §10)
- [ ] **D2 — Interlock/mains one-line diagram.** A required deliverable that
  gates all mains work. (§6)
- [ ] **D3 — Safety-chain fitout:** donor interlock chain, cutoffs, fuse,
  contactor, high-limit, SSR, UV door switch, window film — then re-run the
  step-1 fail-safe proof against the real chain. (§4, §8 step 2)
- [ ] **D4 — TC front-end IC selection + wall-TC channel bring-up**, then
  **workpiece-TC install** (pass-through, panel jack, reflow rack) as
  separate items. (§6)
- [ ] **D5 — UV cure end-to-end.** The first full vertical slice; mostly
  integration once Tracks A–C exist. (§8 step 3)
- [ ] **D6 — Calibration wizard screens** (§20) + **PC-side tools**: log
  decoder (protobuf → DuckDB/CSV/Parquet) and the fit → `oven_cal.h`
  emitter. The PC tools are hardware-independent and can start any time
  after B8's format lands. (§6, §7)
- [ ] **D7 — Reflow bring-up:** PID tuning, calibration runs, drift
  thresholds. (§8 step 4)
- [ ] **D8 — Custom PCB.** (§8 step 5)
- [ ] **D9 — WiFi/OTA** (several PRs): HTTP data server; bundle format +
  validation; ESP32 ROM-loader client; `OtaController` state machine
  (host-testable with fakes); OTA wizard screens; partition-fit
  measurement. (§21, §25, §27; §8 step 6)
