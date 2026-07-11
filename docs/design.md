# Design — Oven Controller (UV Curing & PCB Reflow)

> Working design doc. Built interactively; sections marked **TBD** are open.
> Scope: whole-system architecture. Detailed subsystem docs (safety, control,
> hardware) will hang off this once the skeleton settles.

## 1. Goals & non-goals

**Goals**
- One bench box, two jobs: **PCB reflow** (follow a solder-paste temp curve) and
  **UV resin curing** (gentle heat ~80 °C + UV, on a timer).
- Touchscreen HMI on the CYD where the oven keypad used to be.
- A library of named profiles: one per solder paste, one per resin —
  **editable on-device**, not just authored on a PC.
- Safe by construction: mains heat can't run away even if firmware/SSR fails.
- **Custom controller PCB for deployment**, built around a small/cheap MCU
  (no WiFi/BT needed); the ESP32 is only the bench bring-up target.

**Non-goals (for now)**
- No food use (fumes/residue cross-contamination).
- Not a certified/UL appliance; single-user bench tool.
- No cloud/app; WiFi (if any) is local convenience, not required for operation.
- Reusing the microwave magnetron/HV section — removed, never driven.

## 2. Two-MCU split (the load-bearing decision)

The CYD does **not** have enough free GPIO to run the oven's sensors and loads
(~6 sensors, heater SSR, fan, UV, motor, interlock). So the system is two boards:

- **CYD (ESP32-2432S028) = HMI / "advisor."** Renders UI, reads touch, runs the
  two engines (cure, reflow). Sends requests to the controller; never touches a
  mains load. Treated by the controller as untrusted.
- **Controller MCU (2nd board) = the oven driver + safety authority.** Owns the
  SSR gates, sensor reads, closed-loop temperature control, and *independent*
  safety enforcement. **Two targets:**
  - **Bench/bring-up: ESP32-WROOM-32E** (on hand) — same 3.3 V logic + toolchain as
    the CYD, plenty of ADC/PWM/UART. Fast to iterate.
  - **Deployment: an STM32 (low-cost G0/C0/F103 class) on a custom PCB** — no
    WiFi/BT (the ESP32's radio is dead weight here). STM32 gives superb timers for
    zero-cross/PWM, multi-SPI for the MAX31855 array, a hardware IWDG watchdog, and
    a ROM UART bootloader (flashes cleanly through the CYD if the enclosed board's
    USB isn't reachable). *ATtiny was considered and set aside:* an 8-bit tiny is
    cramped for multi-SPI + framed protocol + PID + safety and weakens the shared
    host-tested-logic story (revisit only if the AVR toolchain is specifically
    wanted). Because the deployment chip differs
    from the bench chip, the controller firmware is written against a **HAL / port
    layer** (mirroring the CYD's `lib/display_port` `IDisplay`/`ITouch` pattern):
    safety + control + protocol logic stay MCU-agnostic and host-testable; only a
    thin per-target adapter (pins, SPI, PWM, timers, watchdog) changes. Board swap
    = new adapter, not a rewrite. Multiple PlatformIO envs select the target.

**Interconnect (DECIDED):**
- **Comms:** a dedicated *second* hardware UART — **CYD GPIO27=TX / GPIO22=RX**
  (CN1) crossed to the controller's UART. 3.3 V both sides (STM32 + ESP32 are 3.3 V
  I/O) → no level shifting. Deliberately *not* P5/UART0, so the CYD's USB serial
  monitor + flashing stay free of contention. Protocol detail in §9.
- **Power:** a mains-derived **5 V PSU on the controller side** powers the
  controller and feeds the CYD via **P5 (VIN + GND only)**; P5's TX/RX pins are
  left unconnected. Common ground is established through P5, so the CN1 comms cable
  only carries the two signal wires. **Heavy loads (UV array, SSR/relay coils,
  motor) get their own PSU rails — never routed through the CYD.**
- **Build caveat:** with the PSU on VIN, don't dual-source 5 V (PSU + USB) while
  flashing the CYD — disconnect the PSU or rely on the board's USB diode.

### Division of labor — smart controller (DECIDED)
The **controller owns profile execution.** The CYD engines build/select a profile
(a time-series recipe), upload it, and issue run-control (start/stop/pause); the
controller sequences the phases, runs the PID/SSR loop, and streams telemetry +
progress back. The two CYD "engines" (cure, reflow) are therefore *profile
producers + run monitors*, not real-time setpoint streamers.

Crash behavior (unchanged by this choice): because safety requires a continuous
heartbeat + `HEAT_EN` from the CYD, a CYD crash/stall still aborts an in-progress
run (heater fails OFF). You cannot both keep the CYD a required safety authority
*and* have a run survive a UI crash. Accepted.

## 3. Layered architecture (safety / control / UI / hardware ports)

Layers, from mains outward:

1. **Hardware safety chain (no firmware):** thermal fuse/cutoff bonded in series
   with the heater + independent high-limit thermostat wired to a contactor coil.
   Removes power even if both MCUs and the SSR fail.
2. **Controller safety supervisor (firmware):** command-timeout (heater needs a
   fresh heartbeat + explicit `HEAT_EN`), hardware watchdog, all outputs default
   OFF on reset (pull-downs on every SSR gate), setpoint clamped to a hard max.
3. **Controller control loop:** PID / zero-cross SSR for the heater, PWM for
   fan/motor, UV on/off.
4. **UART protocol layer:** the "single API" — commands + telemetry + ACK/NAK.
5. **CYD engines (cure / reflow):** profile/timer logic, one per mode.
6. **CYD UI (LVGL):** screens, touch, live status.

Only layers 5–6 live in *this* repo's current firmware. Layers 2–3 are the new
controller firmware.

**Repo structure (DECIDED): one repo, two PlatformIO envs.** The controller gets
its own env + source tree in this repo; the UART protocol (frame/CRC/message
types = the "single API") lives once in a shared `lib/protocol` compiled into both
sides, so the contract can't drift and cross-cutting changes stay atomic. Note the
three-tier native test setup currently assumes CYD-only; it'll need a controller
test lane too.

## 4. Safety model (summary — full detail in a future safety doc)

Defense-in-depth, mains outward. The firmware is **never** the last line.

- **L0 hardware (no firmware):** thermal fuse/cutoff bonded in series with the
  heater; independent high-limit thermostat wired directly to a **contactor coil**
  upstream of the SSR. "SSR modulates, contactor isolates." SSRs fail *shorted*,
  so a firmware OFF cannot be trusted — the contactor + fuse can remove power with
  no MCU involved. **Integrate the donor's existing L0 hardware** (§6 reuse
  inventory): its cavity/element **thermal cutoffs** and its **door-interlock switch
  chain** (primary + secondary + line-shorting monitor switch) — preserve, don't
  bypass or rebuild.
- **L1 command-timeout:** controller keeps the heater/UV enabled only while it has
  a fresh CRC-valid heartbeat **and** an explicit `HEAT_EN` from the CYD (window
  ~500 ms–1 s). No latching on a single "on".
- **L2 watchdog:** hardware watchdog on the controller; on reset all GPIOs default
  to heater/UV **OFF** (pull-downs on every SSR/MOSFET gate).
- **L3 clamps:** setpoint clamped to a hard max; independent high-limit sensor on
  its **own** channel (not the control sensor); bounded total runtime.
- **Mode-dependent temp cap:** each run declares a `mode` (§9); the controller has a
  **compiled-in per-mode cap table** — **cure = 100 °C**, reflow = absolute hard-max.
  Effective cap = `min(absolute_hard_max, modeCap[mode])`; a recipe can only
  *tighten* it, never loosen. So even a buggy/wrong CYD can't push a cure run past
  100 °C (cap lives in the controller's constants — CYD is untrusted). **Software-
  enforced, not hardware-backed:** the L0 cutoff sits at the reflow level, so it
  won't catch a cure-mode runaway to ~150 °C — the firmware clamp + independent
  high-limit hold the cure line. Acceptable (100 °C isn't a fire hazard) but noted.
  The cap is a **firmware constant**, not a user setting.
- **Enclosure over-temp (electronics protection):** on-PCB case sensor (§6),
  two-threshold **warn → auto-abort** to safe state. Protects the SSR/PSU/caps —
  distinct from L0's chamber high-limit, which prevents *fire*; this protects the
  *electronics*. Passive venting only (no active case cooling).
- **Door interlock:** opening the door cuts heater + UV (hardware path preferred,
  mirrored in firmware). Reuse the donor's switch chain (§6). On door-open the
  controller **safes + ends the run** (both modes, stateless); **cure resume is
  reconstructed CYD-side** as a remainder profile (§15).
- **UV:** enclose the source; interlock with the door.
- **Fail-safe default:** heater + UV OFF on boot, reset, crash, brown-out, sensor
  fault, or lost link.

## 5. Control & profiles

**Recipe model (DECIDED): generic multi-channel segments.** A profile is an
ordered list of segments; each segment has a duration and a per-channel target +
interpolation (ramp-linear / hold / step). Channels:

| Channel | Reflow use | UV-cure use |
|---------|-----------|-------------|
| `heater_setpoint_C` | the solder curve (peak ~245 °C) | gentle hold ~80 °C |
| `conv_fan` (on/off or duty) | convection during heat (uniformity) | optional |
| `cool_fan` (on/off) | cooldown boost (donor's magnetron cooling fan) | optional |
| `uv` (on/off or duty) | off | on for the cure |
| `motor` (on/off) | unused | turntable (optional) |

The controller sequences segments, PID-tracks `heater_setpoint_C` via the
zero-cross SSR, sets UV/fans/motor directly, and streams telemetry + progress
(current segment, elapsed, measured temp). Both modes ride the same executor;
"reflow vs cure" is just which channels a profile populates. The recipe also carries
a `mode` tag used *only* by the safety supervisor to pick the per-mode temp cap
(cure = 100 °C; §4) — the executor itself stays mode-agnostic.

CYD engines are profile *producers*: the reflow engine edits/loads a solder curve,
the cure engine edits/loads a resin recipe (temp + UV + timer).

### Domain phase model (DECIDED): ramp(x) + hold(y) → segments
The editor's domain form (§12) is a uniform **three-number phase**: **target temp**,
**ramp seconds `x`** (`x = 0` = as fast as possible), **hold seconds `y`**. Each
phase compiles to two generic segments via the `interp` field:
- **`RAMP_OVER_TIME`** (`x > 0`): ramp the setpoint to target over `x` s (rate
  clamped to achievable, §12).
- **`RAMP_ASAP`** (`x = 0`): drive max heat, **target-gated** — the segment ends when
  the target is reached (duration *estimated* from `oven_cal.h` for the projected
  curve/ETA, but *executed* to target). 
- **`HOLD`**: keep target for `y` s.
Per-phase channel toggles (`conv_fan`/`cool_fan`/`uv`/`motor`) ride along. This is
why the whole projected timeline + ETA are computable up front (§15).

### Characterization / random-profile runs (DECIDED)
For ML data collection, the CYD has a **random-profile generator**: it produces *n*
random profiles of *m* random phases each and runs them back-to-back, logging
everything (§7). Each generated phase randomizes values across **all** channels — a
random heater setpoint, random per-phase on/off of `conv_fan` / `cool_fan` / `uv`,
and a random duration. These are **ordinary profiles through the normal
recipe/executor path** (closed-loop PID on the setpoint), just generated rather than
hand-authored — no special actuator mode needed. `n`, `m`, and the per-channel
ranges are parameters.

**Randomized *within* the safety envelope:** setpoints are clamped to the hard max
and the controller's high-limit / interlock / heartbeat still govern — "random" is
never unbounded.

*Optional future enhancement (not baseline):* an open-loop heater-**duty** channel
mode would inject raw inputs the PID otherwise masks, giving cleaner plant
identifiability for a data-driven estimator (§6). Add only if the random-profile
data proves insufficient.

### Control loop (DECIDED): PI + feedforward + anti-windup
Heater control is **PI, not PID** — implemented as a general PID with `Kd = 0` so
D can be enabled later if peak overshoot proves stubborn. Rationale for this plant:

- The plant is slow, **lag- and dead-time-dominated** (sheathed element's own
  thermal mass → forced air → board mass); PI handles first-order-plus-dead-time
  thermal loops well and D's marginal benefit is small. The convection fan raises
  the heat-transfer coefficient (shorter board τ, more responsive) but the element
  stays hot after the SSR opens — a real overshoot source that anti-windup +
  feedforward address better than D.
- **D amplifies noise** — quantized MAX31855 readings (0.25 °C) next to a
  mains-switching SSR. If ever enabled, D must be **derivative-on-measurement**
  (not on error, to avoid setpoint-step kick) and low-passed hard.
- The main reason to want D (limit **peak overshoot**) is better served by
  **feedforward**: the setpoint trajectory is fully known in advance (scripted
  profile), so a feedforward duty term + PI feedback tracks ramps better than
  leaning on D. Reach for feedforward before D.

Non-negotiables that matter more than P/I/D choice:
- **Anti-windup is mandatory** — the actuator saturates (duty 0–100 %) with dead
  time, so the integrator winds up on ramps; clamp / back-calculate or overshoot is
  guaranteed.
- **Heater is one-sided** (heat only). Cool-down is passive (heater OFF + optional
  fan) → open-loop, not a PID-controlled descent. The loop acts only during
  preheat/soak/reflow.
- **Time-proportioning output:** zero-cross SSR → slow PWM window (~1 s / a few AC
  cycles); PID output = duty.

**UV cure (~80 °C hold)** needs even less — PI, or bang-bang with a small
hysteresis band, suffices for a steady low-temp soak.

## 6. Hardware & I/O (on the controller — full detail in a future hardware doc)

Loads/sensors hang off the controller ESP32 (GPIO-rich); the CYD only does UI.

- **Heater:** a **top-mounted sheathed tubular element** driven via a zero-cross
  SSR (rated ~3.5× element current, heatsinked) + upstream **contactor** + series
  thermal fuse. Gate has a pull-down; may need a transistor/MOSFET to reach the
  SSR's control voltage from 3.3 V. Note the element's own thermal mass = actuator
  lag + a post-shutoff overshoot source (see control loop, §5).
- **Temp sensing (DECIDED — wall TCs + PCB-reference calibration):**
  - **A few fixed K-type thermocouples on the chamber walls**, each on its own
    thermocouple front-end (shared SPI, one CS per channel) → a spatial map of the
    oven. Heat source is a **top-mounted sheathed tubular element + convection
    fan** → a **mixed convection+radiation** oven, *convection-leaning* (forced air
    is the primary, uniform path; the hot element radiates to line-of-sight top
    surfaces, secondary, strongest at peak). Fan mixing makes wall temp a decent
    air proxy, so wall placement is fine; a TC *in the airflow* would be a more
    direct convective proxy, but the reference-PCB calibration absorbs the
    wall-vs-air-vs-board gap regardless.
  - **Detachable reference thermocouple bonded to a scrap PCB**, used **only during
    calibration** to measure true board temperature.
  - One thermocouple type covers both reflow (~245 °C) and cure (~80 °C). The
    front-end IC does cold-*junction* compensation internally; the reference-TC step
    is about board-vs-wall fidelity, not electrical cold-junction.
  - **Board-temp estimator (DECIDED — first-order lag / lumped-RC):** the PID
    controls on an **estimated board temperature**, not raw wall temp:
    `T_board_est = a · LP_τ(T_wall) + b` — a first-order low-pass (time constant τ)
    of the wall temperature plus affine gain/offset. Fit `{a, b, τ}` (optionally
    per-wall weights) by least-squares against the reference-PCB TC over one
    characterization run. This is the minimal model that reproduces the **thermal
    lag** that starves fast ramps and causes cold joints — a static offset/linear
    fit misses it. Bonus: the per-sensor affine term also absorbs each front-end's
    ±2 °C offset (doubles as inter-channel map trim).
    - If a single gain doesn't hold across 80→245 °C (radiation is ∝T⁴,
      nonlinear), **gain-schedule** `{a,b,τ}` via a small temperature LUT — only if
      single-fit residuals are poor.
  - **Calibration workflow (DECIDED — offline fit, compiled into both):** log
    wall-TCs + reference-TC + the random characterization runs to SD (§7); do the
    fit / ML **on a PC**; emit a **generated, committed calibration file**
    (`lib/calibration/oven_cal.h` — `{a,b,τ}` + heat/cool-rate envelopes + hard-max)
    that is **compiled into *both* firmwares.** One source → both binaries identical
    by construction (same single-source pattern as the shared `.proto`); fits the
    matched-pair invariant (§9). Supersedes the earlier "controller NVS" idea.
    - Tradeoff: **recalibration = rebuild + reflash both boards** (no live update).
      Fine for a single bespoke unit. An optional NVS override (firmware defaults,
      NVS supersedes) could be added later if re-tuning gets frequent — not now.
    - Calibration also yields **max heat/cool-rate envelopes** (temperature-
      dependent — heating faster when cold, cooling passive/one-sided), used by the
      feasibility-aware curve preview (§12). Rate-limit + lag math is **shared
      `lib/` logic** reused by the CYD preview and the controller feedforward.
  - **Known limitation:** the model is fit for one calibration board's thermal
    mass; very different boards lag differently and will be mis-estimated. Mitigate
    with a representative calibration board (optionally light/heavy sets selectable
    per run) and conservative peak margins — the estimate doesn't erase board-mass
    dependence.
  - **Open:** exact fixed-channel count; **front-end IC** (undecided — MAX31855 /
    MAX31856 / analog-amp+ADC). (Fit is offline on PC; §10.)
- **Independent high-limit:** separate over-temp sensor/thermostat on its **own**
  channel, distinct from the control/mapping thermocouples above.
- **Enclosure/ambient sensor (DECIDED):** an **on-PCB I²C digital temp sensor**
  (TMP102/LM75-class, ~±0.5 °C) measuring the case interior — *outside* the chamber
  but *inside* the microwave shell, where the electronics live. Purposes: (1)
  **electronics over-temp protection** (SSR wants <~80 °C, plus PSU / caps / MCU),
  (2) an **ambient ML feature** + starting-temp input to the thermal model, (3)
  operationalizes the "keep the SSR cool" monitoring. **Action (DECIDED):
  two-threshold warn→abort** — UI warning + log at a first threshold, **abort the
  run to safe state** at a higher one (protects SSR/PSU/caps). **No active case
  cooling** — passive venting + component placement only. Threshold values TBD.
  Not a thermocouple (electronics range, not reflow range); MCU-internal sensor is a
  poor fallback.
- **UV LED array:** 405 nm via MOSFET (separate from heater).
- **Convection fan (part of *heating*):** LEDC PWM. Runs during reflow heat phases
  for convective uniformity (top-mounted element would otherwise cause top/bottom
  board ΔT + hot spots).
- **Cooling fan (confirmed on donor):** the magnetron cooling fan (runs during/after
  cycles); reuse for cooldown. On/off, separate actuator from the convection fan.
- **Chamber humidity sensor (donor — cure-mode only):** the ML2-STC13SAIT has a
  humidity (steam) sensor for its sensor-cook/reheat modes, sited in the exhaust
  vent. Reusable as a cure-mode ML feature (outgassing/moisture). Caveats: it's a
  **proprietary analog** part (read by the removed control board → characterize the
  interface + read via controller ADC, no datasheet), and **heat-limited (~≤125 °C)
  → not usable during reflow** (would be cooked). Populate `humidity` telemetry only
  in cure mode. (§10)
- **Turntable motor (cure, optional):** the donor's relay-switched low-RPM
  synchronous AC turntable motor — reuse for a cure turntable.
- **Door interlock:** reuse the donor's existing switch chain (see reuse inventory).
- **Noise:** flyback diodes on inductive DC loads, RC snubbers on switched AC,
  decoupling, star ground, adequately-sized shared PSU.

GPIO budget is comfortable on the controller (unlike the CYD), so no I²C expander
is expected — the whole reason for the second board.

### Donor reuse inventory (Toshiba ML2-STC13SAIT — an inverter combo)
The teardown research found subsystems worth reusing rather than rebuilding:

- **Door-interlock switch chain (3–4 switches: primary + secondary + monitor).**
  The monitor switch dead-shorts the line to blow the fuse if the door opens with
  contacts welded. Preserve and integrate into **L0 safety** (§4) — do not bypass.
- **Cavity + element thermal cutoffs/thermostats.** Existing hardware over-temp
  protection → fold into the **L0 chain** (§4) alongside our added thermal fuse.
- **Relay board (12 V coils, transistor drivers).** Reuse relays for the **on/off**
  loads — convection fan, cooling fan, turntable, lamp. Keep our **zero-cross SSR
  only for the modulated heater** (relays can't PWM). Cuts BOM.
- **Cavity NTC thermistor** (donor's convection sensor) — a bonus temperature input
  to log; we still use our own thermocouples for reflow range/accuracy.
- **Low-voltage SMPS (~5 V logic / 12 V relays).** Candidate to power the controller
  + relay coils — may simplify the power topology (§2) vs a new PSU. **Open.**
- **Humidity (steam) sensor** — cure-mode-only, proprietary analog (see above).
- ⚠️ **Remove the inverter/HV/magnetron section** (lethal, holds charge even
  unplugged; qualified-person job). Unused here. (README safety.)

## 7. Persistence & configuration

**Profile storage (DECIDED): CYD onboard flash via LittleFS.** The CYD owns the
profile library; the controller only ever holds the single active run's recipe
(pushed at start of run). (The microSD *is* used — but for data logging, not
profiles; see below. Profiles stay in flash so a run never depends on a card being
inserted.)

**Authoring (DECIDED): on-device editing + PC authoring.** Profiles can be
created/edited on the CYD touchscreen and saved to LittleFS directly. PC authoring
stays useful for seeding versioned defaults and bulk edits. This makes the CYD UI
own a profile editor (§ editing UI below).

**Transfer path (for PC-authored profiles):** with no SD, two mechanisms, not
mutually exclusive:
- **Baseline:** keep default profile JSON in a repo `data/` dir; `pio run -t
  uploadfs` packs it into the LittleFS image. Versioned, simple, needs a USB
  reflash.
- **On-demand:** a serial or local-WiFi upload endpoint to push a one-off profile
  into LittleFS without reflashing.

**Editing UI (DECIDED — full create + edit):** the on-device editor builds/edits
profiles as a uniform per-phase **{target temp, ramp `x`, hold `y`}** form (`x = 0`
= as fast as possible) + per-phase channel toggles, compiling to the generic segment
recipe (§5). Layout designed in §12.

Calibration correction factors (from the reference-TC workflow, §6) live in a
**generated `lib/calibration/oven_cal.h` compiled into both firmwares** (offline PC
fit → committed file → both binaries identical), *not* NVS. See §6.

### Data logging to SD (DECIDED)
Every run is logged to the **CYD's microSD** to build a dataset for offline
analysis / ML (temperatures, duties, fan, etc.). Feeds the future data-driven
board-temp estimator arc (§6).

- **Source:** the enriched `Telemetry` stream (§9) already carries the full raw
  vector; the CYD writes each record to SD (UI graphs a subset). Controller-side
  `ctrlMillis` is the authoritative sample time.
- **Format (DECIDED — binary):** **length-delimited protobuf `Telemetry` records**
  written straight to SD (reuses the shared `.proto`; schema-versioned for free; no
  COBS/CRC needed on a file — just a length prefix). Plus a **header record** per
  file: run-id, profile, run mode, `schemaHash`, controller fwVer, active
  calibration params. A small **PC decoder** uses the same `.proto` → `duckdb` →
  CSV/Parquet.
- **Scope (DECIDED):** **every real run**, **plus artificial random-profile
  characterization runs** (§5 — *n*×*m* random profiles within safety bounds).
  Calibration runs additionally populate `refTemp`.
- **One file per run**, named by run-id + profile.
- ⚠️ **CYD gotcha:** some boards have an **SD ↔ touch SPI conflict** (shared VSPI) —
  verify on this unit. If it bites, options: careful CS/bus arbitration, or buffer
  in RAM and flush between touches.
- **Opens (§10):** wall-clock timestamps without an RTC/WiFi (run-relative time +
  run-id may suffice); SD-write buffering to avoid dropping telemetry.

## 8. Build sequencing (MVP: UV cure first)

First mode to reach end-to-end is **UV cure** (lower stakes than reflow-grade
heat), but it rides on the safety/link foundation, so:

1. **Foundation:** `lib/protocol` + UART link + heartbeat/ACK/NAK; controller
   fail-safe skeleton (outputs default OFF, watchdog, command-timeout) proven with
   a *dummy* load (LED "heater") — it shuts OFF within the timeout when the CYD's
   TX is pulled or the CYD reboots. **No mains yet.**
2. **UV cure path:** CYD cure engine → upload cure recipe → controller runs gentle
   heat (~80 °C) + UV on + timer (+ optional turntable); telemetry + progress back;
   one thermocouple channel live. One full mode working.
3. **Reflow path:** multi-thermocouple mapping, reference-TC calibration workflow,
   PID tuning on a bench heater, the solder curve, live temp graph on the CYD.
4. **Hardware safety chain hardened** before any real mains reflow: contactor,
   zero-cross SSR sizing/heatsink, thermal fuse bonded, independent high-limit.
5. **Port to the deployment MCU + custom PCB:** implement the controller HAL
   adapter for the chosen chip, spin the board, re-verify safety on the real
   hardware. (Bench ESP32 stays as the reference/dev target.)

## 9. UART protocol contract (`lib/protocol`) — the "single API"

Cadence numbers are the accepted defaults (§ table below).

### Link layer (DECIDED)
- UART, **115200 8N1**. CYD **GPIO27=TX / GPIO22=RX** (CN1, second hardware UART,
  *not* UART0) crossed to a spare controller UART; 3.3 V both sides, no level
  shifting. Signal cable carries just TX/RX — common ground comes via the P5 power
  connector (§2 interconnect).
- **Encoding: protobuf payloads (nanopb) framed by TinyFrame.**
  - The `.proto` schema is the **single shared contract** — codegen compiled into
    both firmwares (contract can't drift) and reusable by a future PC
    profile-authoring tool.
  - Protobuf is *not* self-delimiting and carries *no* checksum, so each nanopb
    message rides in the payload of a **TinyFrame** frame, which supplies the
    delimiter + integrity: SOF marker + length + type/ID + **CRC-16** (config
    upgradeable to CRC-32; our frames are small so CRC-16 suffices) + parser resync
    on timeout. Chosen over PacketSerial/COBS because TinyFrame is **plain C, no
    Arduino-`Stream` dependency** → drops into `lib/protocol`, links on ESP32 +
    STM32, and is host-testable. It is *framing-only*, so it doesn't impose its own
    transport semantics — our heartbeat/ACK model (below) stays intact. (MIN /
    tinyproto were passed over: their built-in retransmission is redundant given
    the desired-state hot path self-heals.)
  - Tradeoff accepted: no wire-readability in a plain serial monitor → add a debug
    decode/log path on both ends.
  - Bad CRC → TinyFrame drops the frame; app treats it as a missed tick
    (heartbeat/telemetry) or re-sends (setup commands, via seq/ACK).

### Reliability model (DECIDED): desired-state + heartbeat
- **Hot path (continuous, fire-on-tick, no retransmit):** `Heartbeat` (CYD→ctrl)
  carries `session` + `enable`; `Telemetry` (ctrl→CYD) carries actual state. A lost
  tick self-heals on the next. **`enable=false` or a stale heartbeat → controller
  stops everything** (heater/UV OFF, safe state).
- **Setup path (seq + Ack/Nak, retried until Ack):** recipe upload, `Start`,
  `Stop`, `Calibrate` — they change persistent controller state, so must land
  exactly.

### Message set (protobuf messages)
CYD → controller:
- `Hello{ protoVer, schemaHash }` → handshake at boot (schemaHash gates the link,
  see below).
- `Recipe{ id, mode, repeated Segment{ durMs, heatC, convFan, coolFan, uv, motor, interp } }`
  → upload the whole generic multi-channel recipe in one message (§5). `mode`
  (CURE/REFLOW) selects the controller's per-mode temp cap (§4) — the executor
  stays generic; `mode` is used only by the safety supervisor.
- `Start{ session, recipeId }` → begin a run for this session.
- `Heartbeat{ session, seq, enable, millis }` every **200 ms** → liveness + run
  authorization. Absence/`enable=false` → controller safes.
- `Stop{ session }` (graceful) / `Abort{}` (immediate safe, sessionless).
- `Calibrate{ … }` → reference-TC calibration hooks (§6, TBD).

*(No `Resume` command — cure resume is a fresh `Start` of a CYD-generated remainder
profile; the controller stays a stateless executor, §15.)*

controller → CYD:
- `Hello{ fwVer, caps, schemaHash }` → firmware id, capabilities, schema fingerprint.
- `Ack{ seq }` / `Nak{ seq, reason }` → setup-command result (Nak on bad CRC /
  out-of-range / illegal transition).
- `Telemetry{ session, seq, ctrlMillis, repeated wallTemp, refTemp, boardEst,
  caseTemp, humidity, doorOpen, setpoint, heaterDuty, convFan, coolFan, uvDuty, motor,
  segIdx, elapsedMs, runState, faultCode }` every **250 ms** (4 Hz). `runState`
  (IDLE/RUNNING/DONE/FAULT) drives the CYD's run/summary screens — **no PAUSED**
  (the controller has no pause state; "paused" is a CYD-side UI concept, §15).
  Carries the **full raw vector** (not just a UI subset):
  it doubles as the **SD log record** (§7) — the UI graphs a subset, the CYD writes
  the whole message. `ctrlMillis` is the controller-side sample timestamp (timing
  source of truth). `refTemp` is populated only when the calibration TC is attached.
  Rate is **~1 Hz when idle** (keeps Home's chamber temp + link indicator live, §14)
  and **4 Hz during a run**. The controller also sends an **immediate unsolicited
  telemetry on `doorOpen` change** so the CYD can wake the display fast (§17).
- `Done{ session }` → profile finished cleanly.
- `Fault{ session, code }` → run aborted to safe state (over-temp, sensor fault,
  lost link, …).

### Session & safety semantics
- **Enable = fresh HB for the active session.** No latch. Command-timeout **750 ms**
  (~3–4 missed HBs) → heater/UV OFF + safe state.
- **Session id** prevents a rebooted/stale CYD from authorizing a run it doesn't
  know about (it has no session post-reboot → controller times out → safe).
- Controller **clamps** every recipe to the hard max and NAKs out-of-range uploads;
  it never trusts the CYD past a limit.
- Independent of the protocol entirely: the L0 hardware chain (§4) still removes
  power if firmware/SSR fail.

### Schema-consistency check (MAVLink CRC_EXTRA-style) — DECIDED
Plain link CRC catches bit errors but not *version skew*: a frame from a firmware
built against a different `.proto` can transmit with a perfect CRC and be
mis-decoded. Two mechanisms guard against that:

1. **Handshake schema-hash gate (strict, fail-closed).** A build-time fingerprint
   of the shared `.proto` (hash of its compiled `FileDescriptorSet`) is baked into
   both firmwares as a constant and exchanged in `Hello.schemaHash`. **Mismatch →
   the controller refuses to leave safe state and the CYD shows an explicit
   "controller/UI schema mismatch" error.** Byte-exact: *any* schema change breaks
   the link until both boards are reflashed. This deliberately trades away
   protobuf's rolling-upgrade ability for fail-closed simplicity — acceptable
   because **the CYD and controller are always flashed as a matched pair**
   (invariant). Clearer than MAVLink's silent frame-drop: you get a diagnostic.
2. **Message-type id mixed into the frame CRC (per-frame).** The message type is
   folded into TinyFrame's checksum input (via `TF_CKSUM_CUSTOM16`), so a frame
   decoded as the wrong message type fails CRC. Cheap belt-and-suspenders against
   type confusion, checked on every frame.

(Full MAVLink-style per-frame *schema* seeding was considered and dropped as
redundant on a point-to-point link once the handshake gate passes.)

### Code shape
`lib/protocol` = pure C++ (no Arduino/LGFX): frame encode/decode, CRC, recipe
(de)serialize + range-validate, and the run state machine. Host-tested in
`native_logic`. The real UART sits behind a transport port (an in-memory pipe in
tests). Linked into **both** firmwares so the contract can't drift.

### Cadence/params (DECIDED — accepted defaults)
| Param | Value | Note |
|-------|-------|------|
| Baud | 115200 8N1 | safe over short wires; recipe upload is tiny |
| Heartbeat period | 200 ms | CYD→controller liveness+auth |
| Command-timeout | 750 ms | miss ~3–4 HBs → safe |
| Telemetry rate | 250 ms (4 Hz) | smooth live temp graph |
| Setup ACK timeout / retries | 200 ms / ×3 | then surface a UI error |
| CRC | CRC-16/CCITT | over frame body |

## 10. Open questions

- **Deployment MCU:** STM32 chosen; pick the exact line/package (G0 vs C0 vs F103,
  hand-solder-friendly package). Drives the PCB + HAL adapter. (§2)
- **Controller PCB:** thermocouple front-ends (N × MAX31855), SSR/UV/contactor
  drivers, connectors, on-board vs off-board safety chain, flashing/debug header
  (SWD + UART-bootloader access).
- **Profile editor UI:** structure/layout now designed (§12); remaining detail —
  advanced-mode reorder UX, per-field keypad ranges, and building it in the sim.
- **Profile transfer:** `data/` + `uploadfs` baseline, and/or a serial/WiFi
  on-demand push? (§7)
- **Thermocouples:** exact fixed-channel count; **front-end IC** (MAX31855 vs
  MAX31856 vs analog-amp+ADC); whether the `{a,b,τ}` fit runs on-device or on a PC.
  (Estimator model, PID source, NVS storage now decided — §6.)
- **`.proto` + deps:** finalize the schema fields; add `nanopb` + `TinyFrame` to
  both PlatformIO envs and wire up the protobuf codegen step. (§9 — contract is
  designed; this is the mechanical follow-through.)
- **Controller USB access once enclosed:** the *other* linked doc covers flashing a
  2nd MCU through the CYD — only needed if the controller's own USB isn't reachable.
- **Controller test lane** in the three-tier native/embedded setup.
- **UV cure specifics:** is a turntable in scope for MVP; UV as on/off or PWM duty;
  door-interlock wiring for the UV enclosure.
- **Temp-cap constants:** cure = 100 °C (set); reflow absolute hard-max value (near
  the element/SSR/thermal-fuse limit) TBD; both firmware constants (§4).
- **Deviation/drift thresholds:** live-cue band (§15) and end-of-run
  calibration-drift trigger (§16) — sustained-time N + RMSE/max thresholds; firmware
  constants, tune against real runs.
- **Sleep constants (§17):** idle timeout (~1–2 min) and the safe-touch temperature
  below which sleep is allowed while cooling.
- **Auto-brightness tuning (§18):** LDR→backlight curve/LUT, min-floor/max-ceiling,
  filter/ramp time-constants — tune on real glass.
- **Cure resume (§15):** paused-state timeout before the CYD discards the remainder;
  whether Resume needs the full press-and-hold (§19) or a plain tap suffices.
- **Calibration presets (§20):** the Quick/Standard/Thorough scope definitions
  (`n`/`m`/coverage/time).
- **Donor reuse (Toshiba ML2-STC13SAIT):** characterize the **humidity sensor's**
  analog interface (cure-mode only); decide **relay-board reuse** for on/off loads
  vs new drivers; decide whether to **reuse the donor SMPS** (5 V/12 V) for the
  controller + relay coils vs a new PSU (§2 power topology); whether to log the
  donor **NTC** as a bonus input. (§6)
- **Data logging + calibration pipeline:** the PC-side protobuf→CSV/Parquet decoder
  tool; the analysis that emits the generated `lib/calibration/oven_cal.h`
  (`{a,b,τ}` + heat/cool-rate envelopes) compiled into both firmwares (§6);
  wall-clock timestamps without an RTC/WiFi (run-relative + run-id may suffice);
  SD-write buffering; verify the CYD's SD↔touch SPI coexistence on this unit. (§7)
- **Random-profile generator:** ranges/`n`/`m` defaults; pure-random vs structured
  sampling (e.g. Latin-hypercube) for better coverage. (§5)

## 11. Controller firmware structure (HAL)

Same shape as the CYD's `lib/display_port` (`IDisplay`/`ITouch`) pattern, on the
other side of the UART. A **hardware abstraction layer** splits the controller
firmware into portable logic vs per-chip hardware access, so the same tested logic
runs on the **bench ESP32** and the **deployment STM32**, and on the **host** with
fakes.

### Layering
- **`lib/control_port/`** — pure C++ **interfaces** (ports), one per hardware
  capability. No vendor headers → compiles anywhere (incl. `native_logic`).
- **`lib/control_logic/`** — **portable** logic depending only on the ports: the
  safety supervisor, profile executor, PID + feedforward, time-proportioning,
  calibration application, protocol glue. Host-tested.
- **Adapters** (in each firmware env's `src/`): `Esp32*` (Arduino-ESP32: LEDC PWM,
  SPI for MAX31855, `millis()`, ESP watchdog, Preferences/NVS) and `Stm32*` (timers,
  SPI, IWDG, flash). Thin — just peripheral calls, no decisions.
- **`Fake*` adapters** (test tree): record/inject values; `FakeClock` advanced by
  hand. Let `native_logic` unit-test safety/control deterministically, no board.

`main()` on each target constructs the real adapters and injects them into the
logic; tests inject fakes into the **same** logic. Injection site is the only
divergence between targets.

### Ports (capabilities)
`IThermocouples` (wall array + reference, per-channel `{celsius, fault}`) ·
`IHeaterSwitch` (bare on/off SSR gate — see below) · `IContactor` ·
`IUvOutput` · `IFanOutput` (conv + cool) · `IMotorOutput` · `IDigitalIn` (door
interlock, high-limit) · `ICaseTempSensor` · `IHumiditySensor` · `IClock`
(fakeable time — critical for testing timeouts/PID `dt`) · `IWatchdog` ·
`ICalibrationStore` (NVS) · `ISerialTransport` (the protocol transport, §9).

### Detailed sketch — heater / time-proportioning

**Key decision: the time-proportioning lives in *logic*, and the port is a bare
on/off switch.** A zero-cross SSR can only switch at AC zero crossings, so heater
"power" is done by **time-proportioning** ("slow PWM"): over a ~1 s window, hold the
SSR on for `duty × window`. We put that algorithm in `control_logic` (so it's
testable with a `FakeClock` + a recording switch) and keep the port trivial (a
`digitalWrite`). The alternative — `setDuty()` on the port, realized by hardware
PWM in the adapter — would bury testable *policy* (window, min on/off) in the
adapter and diverge per chip. We don't do that.

The seam is a bare switch:
```cpp
// lib/control_port/
class IHeaterSwitch {                 // adapter = GPIO to the zero-cross SSR gate,
 public:                              //          with a pull-down (fail-safe OFF)
  virtual void set(bool on) = 0;
  virtual ~IHeaterSwitch() = default;
};
```

Time-proportioning is a logic class driven by the PID's duty:
```cpp
// lib/control_logic/
class HeaterActuator {
 public:
  struct Config {
    uint32_t windowMs = 1000;  // proportioning period
    uint32_t minOnMs  = 50;    // duties below this snap to full-OFF (SSR/thermal floor)
    uint32_t minOffMs = 50;    // duties above (window-minOff) snap to full-ON
  };
  HeaterActuator(IHeaterSwitch& sw, IClock& clock, Config cfg);

  void setDuty(float d0to1);   // from PID + feedforward (§5); clamped 0..1
  void tick();                 // call every control loop; (re)drives the switch
  void forceOff();             // safety override: immediate OFF, duty := 0
};
```
`tick()` behavior:
- **Latch duty per window** (start a new window every `windowMs`; hold that window's
  on-time constant) so a mid-window duty change can't glitch the output.
- `onMs = clamp(duty × windowMs, …)`; **snap** `onMs < minOnMs → 0` and
  `onMs > windowMs − minOffMs → windowMs` (clean 0 %/100 %, no sliver pulses).
- Switch **on** while `(now − windowStart) < onMs`, else **off**.
- Zero-cross note: each commanded transition snaps to the next AC zero crossing (a
  few ms) — thermally negligible over a 1 s window, so **no cycle-counting or
  zero-cross-detect input is needed** in the port.

**Safety override & liveness:** `SafetySupervisor.tick()` runs first each loop; on
any fault/interlock/stale-heartbeat it calls `heater.forceOff()` (and opens the
contactor) — the actuator never overrides safety. If the loop *hangs*, `tick()`
stops running and the pin would hold its last state, so the **hardware watchdog**
(→ reset → pull-down → OFF) is the backstop, not the actuator itself.

**Testability example:** `FakeClock` + recording `IHeaterSwitch`; `setDuty(0.3)`,
`windowMs = 1000`; advance the clock and assert the switch was on ~300 ms per 1 s
window; assert `0.0`→always off, `1.0`→always on, `0.01`→off (below `minOnMs`).

## 12. Profile editor UI (CYD)

Designed against the **ui-development** skill's rules (320×240 resistive, gloves,
hazardous machine). Screens live in `lib/ui_logic/`, host-testable.

### Key insight: edit parameters, not the curve
The design rules forbid gestures/drag and mandate constrained numeric input — so you
**don't drag points on a graph**. Instead you edit named phase **parameters** via
steppers/keypad, and a **read-only curve preview** is *derived* from them. "Curve
editor" = parameter editor + non-interactive preview.

### Structure (DECIDED): fixed templates + advanced add/remove
- **Default — fixed phase templates per mode.** Reflow: *preheat / soak / reflow /
  cool*; cure: *warm / cure / cool*. You edit each phase's parameters + per-phase
  channel toggles, not the structure. Matches how solder profiles are specified,
  fits the screen. Create-from-scratch = a new profile seeded from the default
  template. Phases compile to the generic multi-channel segment recipe (§5).
- **Advanced (progressive disclosure)** — add / remove / **reorder** generic
  segments via up/down buttons (no drag). For non-standard profiles; more ways to
  make an invalid one, so it's behind an explicit "Advanced" affordance.

### Navigation: hub-and-spoke (two screens)
**Overview** — the curve is the one primary job:
```
┌──────────────────────────────────────┐
│ Edit: LF-245            ● IDLE        │  header + machine state
│  °C 250│    __                        │
│        │  _/  \_                      │  read-only derived curve
│      25│_/     \__                    │
│        └──────────── t                │
├───────────────────┬──────────────────┤
│ PREHEAT →150 @1.5 │ SOAK 150–180 90s  │  2×2 phase tiles (~155×88 px),
├───────────────────┼──────────────────┤  tap → edit. (Advanced: this
│ REFLOW peak245 45 │ COOL −2.0/s       │  becomes a scrollable list with
├───────────────────┴──────────────────┤  up/down + add/delete.)
│ ‹ Back                     Save ✓     │
└──────────────────────────────────────┘
```
**Phase editor** — one phase, prev/next in the header:
```
┌──────────────────────────────────────┐
│ ‹ Soak  (2/4)                    ›    │
│  Target      [ − ]  165 °C  [ + ]     │  stepper (~60 px); tap the
│  Ramp        [ − ]   MAX    [ + ]     │  number → constrained keypad
│  Hold        [ − ]   90 s   [ + ]     │  (Ramp at min = MAX / ASAP)
│  Conv fan[ON]  Cool fan[OFF]  UV[OFF] │  per-phase channel toggles
│  Cancel                     Done      │
└──────────────────────────────────────┘
```

### Design-rule compliance
- **Numeric:** steppers (min/max, disabled at limit) + tap-value→**constrained
  keypad** for wide ranges. No free text (except the profile **name** via an
  on-screen keyboard, used rarely on save-as).
- **Safety-limit clamp:** stepper ceilings = the **mode cap** — **100 °C in cure**,
  the reflow hard-max in reflow (§4). UI prevents entering an over-limit value; the
  controller still clamps/NAKs as backstop (§9).
- **Validation, two tiers:**
  - *Hard-invalid* (over hard-max temp; peak ≤ soak ≤ preheat; non-monotonic times)
    → **red + word**, **blocks Save**.
  - *Physically-optimistic* — a ramp faster than the oven's calibrated max heat/cool
    rate → **amber + word**, draws the achievable curve, **allows Save** (the oven
    does its best; the user should see it but isn't blocked). (DECIDED.)
- **Idle-only:** the editor is unreachable during a run → no STOP button here (STOP
  lives on the run/monitor screens per the rules).
- Cure mode reuses this exact structure (phases warm / cure / cool).

### Feasibility-aware curve preview (DECIDED)
The preview is **not** a naive plot of the entered setpoints — it incorporates the
oven's **calibrated capability**:
- **Requested** trajectory drawn as a ghost/dashed line (what you entered).
- **Achievable** trajectory drawn solid — the requested line **rate-limited** by the
  calibrated **heat/cool-rate envelopes** (temperature-dependent; cooling is the
  binding constraint since the heater is one-sided). Where they diverge, the phase
  is flagged (amber, see validation).
- Calibration constants come from the **compiled-in `oven_cal.h`** (§6) — no runtime
  message; the CYD computes the preview locally so it stays instant as steppers
  change. The rate-limit/lag math is **shared `lib/` logic**.
- **Uncalibrated fallback:** a fresh oven with only default constants shows the
  idealized linear curve with an "uncalibrated — preview is idealized" note.
- Read-only chart (no gauge-without-readout, no 3D/gradients).
- *(Optional later: run the setpoints through the full `{a,b,τ}` closed-loop model
  to also show overshoot/soak-settling — the shared model is built to extend to it.)*

## 13. UI navigation map & global chrome

Hub-and-spoke per the **ui-development** skill. Largely **hardware-independent** —
screens talk to the controller via the protocol + capabilities, not specific ICs —
so the UI can be designed before the teardown. Variable channels (cool_fan,
turntable, humidity) are gated by the controller's reported capabilities.

### Screen map
```
Home / Status  (idle hub — machine state + chamber temp)
├─ UV Cure  ─► Cure Setup   ─► Confirm ─► Cure Run/Monitor ─► Summary
├─ Reflow   ─► Reflow Setup ─► Confirm ─► Reflow Run/Monitor ─► Summary
├─ Profiles ─► Profile library ─► Profile Editor (§12)   [New / Edit / Delete]
├─ Calibrate ─► Calibration workflow ─► characterization run
└─ Settings ─► units / thresholds / WiFi / about / advanced toggle
```

### Global chrome (every screen)
- **Header:** screen title + **machine-state badge** (IDLE / HEATING / HOT / CURING
  / FAULT) + **link indicator** (controller connected & schema OK?).
- **Footer:** Back (left) + **STOP** (large red, right) — present + armed on every
  *running* screen; never blocked by UI work (machine work is off the LVGL loop).
- **Always visible:** machine state + link status (operator must never be unsure
  whether heat/UV is on, or whether the controller is responding).

### Cross-cutting overlays / states
- **Fault / alarm:** modal red overlay on controller `Fault` (§9); outputs already
  safe; shows code + plain-language cause; [Acknowledge].
- **Link lost / schema mismatch:** persistent header warning; **Start disabled** —
  no run may begin without a healthy link (safety). Mirrors the §9 gate.
- **Hazardous start confirmation** sits between Setup and Run (arm-then-start /
  press-and-hold; safe option default; red reserved for the hazardous verb).

## 14. Home / Status hub screen

Root of the hub-and-spoke; **sets the visual language** for every other screen.
Primary job: pick what to do, while making the machine's safety state unmissable.

```
┌──────────────────────────────────────┐
│ Oven Controller               ⬤ Link  │  header: name + link indicator
├──────────────────────────────────────┤
│   ● IDLE            Chamber  24 °C     │  status band (state + chamber temp)
├───────────────────┬──────────────────┤
│      ☀            │       ⧉           │  two big mode buttons
│    UV CURE        │     REFLOW        │  (~155×110 px, primary, isolated)
├──────────┬────────┴────┬─────────────┤
│ Profiles │  Calibrate  │  Settings   │  secondary row (≥56 px tall)
└──────────┴─────────────┴─────────────┘
```

### State treatments (where the safety UX lives)
- **Idle & cool:** `● IDLE` (green) + neutral chamber temp.
- **Hot after a run:** band → `⚠ HOT — Chamber 118 °C (cooling)`, **amber/red + the
  word HOT** (never colour alone). Mode buttons stay enabled (set up next run while
  it cools).
- **Link lost / schema mismatch:** red header indicator + body banner `⚠ Controller
  not responding`; **mode buttons disabled** — no run flow without a healthy link
  (mirrors §9).

### Visual language established for all screens
- Header = title + machine-state badge + link dot. **The root hub is the sole
  exception to the footer rule:** no Back (it's root), no STOP (idle) — secondary
  actions live in the bottom row instead.
- Grayscale base; **colour only for state** (green idle · amber heating/hot · red
  fault); mode tiles neutral; danger-red reserved.
- Big-numbers-first readouts.
- Mode buttons open a *flow* (→ Setup), not heat directly — not themselves hazardous.

### Behavior implication (DECIDED)
The controller streams **low-rate idle telemetry (~1 Hz)** even when not running, so
Home always shows live chamber temp **and** keeps the link indicator honest. Cheap
protocol addition (idle heartbeat/telemetry) on top of §9.

## 15. Run / Monitor screen

The most safety-critical screen. Primary job: watch the run progress safely, with
the **projected-vs-actual curve** as the centerpiece.

```
┌──────────────────────────────────────┐
│ Reflow: LF-245        ⚠ HEATING   ⬤   │  profile + state badge + link
├───────────────────────┬──────────────┤
│  212 °C   ▲set 210     │ Soak  2/4    │  big actual temp + setpoint · phase
│                        │ ETA  4:32    │  · ETA
├───────────────────────┴──────────────┤
│ °C 250│          projected ·····      │
│       │        ____·····              │  CHART (the star):
│       │  actual                       │   ···· projected (achievable)
│    25 │_/                             │   ─── actual (live boardEst)
│       └───────────┃─────────── t      │   ┃ now-marker
├──────────────────────────────────────┤
│ ▓▓▓▓▓▓▓▓▓░░░░░░░░░░░░░  46%            │  progress bar
├──────────────────────────────────────┤
│            ■   S T O P                │  large red STOP (immediate)
└──────────────────────────────────────┘
```

### Phase model (DECIDED): ramp(x) + hold(y)
Each phase = (1) **reach a target temp over `x` s** (`x = 0` → as fast as possible),
then (2) **hold the target for `y` s**. Uniform three-number form across all phases;
compiles to two generic segments (ramp + hold, §5).
- **Ramp:** `x = 0` → drive max heat, **target-gated** (advance when target reached);
  `x > 0` → ramp the setpoint over `x` s (rate `ΔT/x`, clamped to achievable if too
  fast → warn, §12).
- **Hold:** `y` s at target (time-based).
- This is the "hybrid" advance policy made precise: **ramp target-gated, hold
  time-based** — guarantees the soak/reflow happens *at* temperature.

### Projected curve + ETA, computed from calibration (DECIDED)
- Every phase duration is computable from the compiled-in `oven_cal.h` (§6): an ASAP
  ramp time = `∫ dT / heatRate(T)` across the ramp (numeric — rate is temp-dependent);
  cool phases use the cool-rate envelope; `x > 0` uses `x` (or the achievable time if
  clamped); holds use `y`. → the **whole projected timeline is known up front** →
  **ETA is a real countdown**, computed entirely on the CYD.
- Because ASAP ramps are **target-gated**, ETA is a calibration-based **estimate that
  slips live** if the oven lags (cold board, door, drift) — re-estimated from progress
  each tick. Honest, not fictional.
- **Projected** (dotted, the achievable §12 timeline) vs **actual** (`boardEst` from
  `Telemetry`, 4 Hz, bold, start→now with a now-marker). Plot `boardEst` — same
  board-temp terms as projected.
- **Deviation cue:** `|actual − projected|` beyond a band → readout **amber** (run
  falling behind / overshooting) — distinct from the L0/L3 over-temp trips.

### Chrome & controls
- Header: profile name + machine-state badge + link dot.
- Big current-temp readout + setpoint; phase name + count; ETA.
- **STOP:** large, full-width, **immediate** (emergency — no confirm); aborts to safe
  state (heater/UV off, contactor open). Never blocked (machine work is off the LVGL
  loop).
- UV / fan / turntable state indicators.
- Fault → the modal alarm overlay (§13).

### Cure variant
Same skeleton, but: the curve is gentle (→ 80 °C hold, capped 100 °C §4), the
**`UV ON`** indicator is prominent red (eye-safety), and the **countdown/ETA is the
star** (cure is mostly a timer); turntable indicator if present.

### Door-open during a run (DECIDED): CYD-orchestrated resume, stateless controller
The hardware interlock cuts heat + UV **instantly** regardless (L0, §4). On door-open
the controller **autonomously safes and ends the run to idle** — same for both modes,
**no pause state, no resume logic, no context retained** (it stays a stateless
profile executor). All resume intelligence lives on the CYD:

- **Reflow → aborted.** The CYD shows "Run aborted — door opened"; no resume (reflow
  can't survive the thermal excursion — decided).
- **Cure → CYD-orchestrated resume.** The CYD, having tracked progress from telemetry
  (phase, elapsed, UV-dose), shows a Paused overlay:
  ```
  ⏸ PAUSED — Door open · UV OFF
  Close the door, then Resume.        [ Abort ]   [ Resume ▶ ]
  ```
  On **Resume** (explicit tap, enabled only when the door is closed) the CYD
  **generates a remainder profile** — an `RAMP_ASAP` re-heat to the current target +
  the remaining hold/phases + remaining UV dose — and **`Start`s it as a fresh run.**
  To the controller it's just a new profile.
  - **Why CYD-side:** controller stays stateless → robust (a controller reset
    mid-pause loses nothing; the CYD re-sends). Reuses the phase/segment model + the
    profile generator; the board's cool-down is handled for free by the ASAP re-heat
    ramp at the front of the remainder.
  - **Resume re-energizes UV** → explicit + door-closed-gated (no auto-resume, eye
    safety). Whether it needs the full press-and-hold (§19) is a minor open.
  - The **remainder-profile generator** is CYD logic (host-testable).
  - **Paused-state timeout:** the CYD discards the paused state (and the remainder)
    after a timeout (constant, §10); a lost heartbeat also ends it (the controller is
    already idle).

## 16. Run summary / results screen

Shown at end of run (Run/Monitor → `Done` → Summary; also after Stop/Fault). Primary
job: report the outcome and **surface calibration drift**.

### Contents
- Outcome badge: **Completed / Stopped / Fault** (+ cause).
- Full-run **projected-vs-actual overlay** (both curves complete).
- **Fit verdict** (Good / Fair / Poor) + the key numbers (max/RMS deviation;
  per-phase target hits).
- Actions: **Run again** · **Home** (the SD log is already written).

### Calibration-drift advisory (DECIDED)
On a **completed** run only (abort/fault skip it — data incomplete), the CYD compares
actual vs projected using the **same residual math as the live cue (§15)**, shared
`lib/` logic:
- **Aggregate residual** — RMSE, or deviation beyond a band **sustained > N s**
  (sustained, so a transient door-open spike doesn't trip it) — **and**
- **Per-phase target checks** — did soak/peak actually reach target and hold
  time-above-liquidus?

Beyond a threshold constant → a prominent but non-alarming advisory with a shortcut
into the **Calibration workflow**:

> ⚠ Actual temperature differed from the prediction. This **may** mean the oven
> needs recalibration — or that this board's thermal mass differs from the
> calibration board.

**Honest by design:** high deviation doesn't prove oven drift — per §6, a different
board mass causes it too — so the wording says "may" and names both causes rather
than pushing the user straight to recalibrate.

**Tie-ins:** the fit metric is written to the **SD log header** (§7) so a flagged run
becomes a good recalibration-dataset candidate for the offline ML; threshold value is
a firmware constant (TBD, §10).

## 17. Idle sleep & wake

**Sleep = backlight off, MCU + UART link stay alive (DECIDED)** — *not* ESP32 deep
sleep. The rig is mains-powered, so the goal is **display longevity + not glaring**,
not power. Backlight-off gives instant wake, no LVGL state loss, and keeps the link
honest (so state is current on wake and door-wake is a simple event).

- **Idle-only (safety):** the inactivity timer runs **only when idle**. The
  Run/Monitor screen keeps the display awake for the whole run — sleeping mid-run
  would stop the heartbeat → controller aborts to safe (§9). **Never sleep during a
  run.**
- **Suppress sleep while HOT (DECIDED):** stay awake until the chamber cools below a
  safe-touch threshold, so the `HOT` warning (§14) stays visible to anyone
  approaching. Sleep only when **idle AND cool**.
- **Inactivity timeout:** ~1–2 min default, configurable (Settings / constant, §10).
- **Wake sources:**
  - **Any touch** — the wake-tap is **consumed** (lights the screen without also
    actuating the control beneath it).
  - **Door-open event** from the controller.
  - **Incoming fault/alarm** — never hide a fault behind a dark screen.
- **Requires door state on the CYD:** the door interlock is on the controller/mains
  side (§6), so the controller **reports door state over UART** — a `doorOpen` bit in
  telemetry **plus an immediate unsolicited telemetry on change** for low-latency
  wake (§9).

## 18. Auto-brightness (ambient light)

Phone-style automatic screen brightness — a self-contained CYD feature (no
controller/protocol involvement).

- **Hardware:** the CYD's **LDR on GPIO34** (ADC1 — reads fine with WiFi on) senses
  ambient light; the **backlight on GPIO21** (LEDC PWM / LovyanGFX `setBrightness`)
  is the output.
- **Minimum brightness floor (SAFETY):** clamp to a **readable minimum** so
  `HOT` / `UV ON` / fault state stays legible even in a dark shop, and a **max** for
  sunlight readability. Auto-brightness reinforces the design-rule contrast
  requirement rather than fighting it.
- **Smoothing + hysteresis:** low-pass the LDR, add hysteresis, and **ramp** the
  backlight smoothly (ease, don't jump per sample). Guards against noise, a hand
  passing over, and the backlight's own glow reaching the same-side LDR (feedback).
  Sample at a few Hz.
- **Perceptual mapping:** perception is logarithmic → use a curve/LUT, not a linear
  map. Start with a few breakpoints; **tune on real glass** via the ui-development
  device loop (`dev-shot`).
- **Interaction with sleep (§17):** independent — auto-brightness governs the
  *awake* backlight; sleep turns it off; on wake it resumes.
- **Optional Settings:** auto on/off + a manual brightness bias (detailed with the
  Settings screen); the safety min-floor always applies.
- **Code:** ports `IAmbientLight` (read) + `IBacklight` (set); a pure, host-testable
  `AutoBrightness` class (filter + curve + hysteresis + clamp) in `lib/` — mirrors the
  HAL/MVVM pattern.
- **Opens (§10):** the curve/LUT + floor/ceiling + filter time-constants (tune
  on-glass).

## 19. Setup & hazardous-confirm screens

The safety gate: **Setup** (choose + review, with readiness checks) → **Confirm**
(deliberate, specific, hard to trigger accidentally). Strictest application of the
ui-development rules.

### Setup
```
┌──────────────────────────────────────┐
│ ‹ Reflow Setup            ● IDLE  ⬤   │  mode + machine state + link
├───────────────────────┬──────────────┤
│ Profile: LF-245        │ Peak 245 °C  │  selected profile + key facts
│ [ Change ]  [ Edit ]   │ Est  6:10    │  (est total from calibration §15)
├───────────────────────┴──────────────┤
│ °C 250│      __                       │  feasibility-aware preview (§12)
│    25 │_ /  _/  \_ ___                │
│       └──────────────── t             │
├──────────────────────────────────────┤
│ ⚠ Door open — close to start          │  readiness line (conditional)
├──────────────────────────────────────┤
│ ‹ Back                  Start ▶       │  → Confirm (disabled if not ready)
└──────────────────────────────────────┘
```
- `[Change]` → Profile library; `[Edit]` → editor (§12). Est total is free from
  calibration (§15).
- **Readiness gating:** `Start` enabled only when the **link is healthy** (§13) *and*
  the **door is closed** (`doorOpen` telemetry, §9/§17) — the hardware interlock
  enforces it regardless, but the UI shouldn't offer an un-runnable Start; it says
  why when blocked. `Start` always routes to **Confirm**, never straight to heat.

### Confirm (hazardous)
```
┌──────────────────────────────────────┐
│ Confirm — Reflow                      │
│  Start reflow?                        │
│  Heater to 245 °C · ~6 min · LF-245   │  specific statement (temp + time)
│  ⚠ Hot surfaces & fumes. Ventilate.   │  hazard warning
│     Don't leave unattended.           │
├───────────────────┬──────────────────┤
│     Cancel        │  ██ HOLD to ██    │  Cancel easy/safe · Start red +
│                   │  ██ Start Heat ██ │  press-and-hold
└───────────────────┴──────────────────┘
```
- **Specific statement** ("to 245 °C · ~6 min"), **verb on the button** ("Start
  Heating"), **red reserved** for the hazardous action, and the **safe way out
  (Cancel) is the easy one** while committing takes deliberate effort.
- **Start gesture (DECIDED): press-and-hold ~2 s with a fill ring, both modes**
  (reflow = highest thermal energy; cure = UV eye hazard). Releasing early aborts.
- **On commit:** the CYD uploads the recipe + sends `Start{session, recipeId, mode}`
  (§9), then → Run/Monitor (§15).
- **Cure variant:** statement "UV on · 80 °C · 30 min"; warning "⚠ UV light — protect
  eyes, keep enclosure closed"; button "Start Cure"; same door/enclosure-closed gate.
- Confirmations are kept to **only** the hazardous start, so they retain force.

## 20. Calibration workflow

Produces the data behind `oven_cal.h`. **Honest framing:** because the fit/ML is
**offline on the PC** and calibration is **compiled into both firmwares** (§6), this
on-device flow **collects + logs data — it does not compute or apply the
calibration.** It ends by handing off to the PC. It unifies the random-profile
characterization (§5), the reference TC (§6), and SD logging (§7).

A step-wizard:

1. **Attach & verify.** Prompt: attach the reference TC to a scrap PCB, place in
   chamber, close door. **Verify the reference TC reads valid** (`refTemp` not
   MAX31855-faulted) and the door is closed before `Next` enables.
   ```
   Reference TC:  24.1 °C  ✓ detected
   Door: closed ✓                        [ Cancel ]  [ Next ▶ ]
   ```
2. **Scope preset** — Quick (~15 min) / Standard (~45 min, recommended) / Thorough
   (~90 min); maps to the random-generator `n`/`m`/coverage (§5, values §10).
3. **Hazardous confirm** — press-and-hold (§19): it drives the oven across the **full
   temperature range** for a long time (needed to characterize the heat/cool-rate
   envelopes end to end), bounded by the safety envelope.
4. **Run** — like Run/Monitor but for the sweep; projected-vs-actual is meaningless
   for random profiles, so it shows **live wall TCs + reference(PCB) temp** (watch the
   lag), run `k/N`, ETA, and a persistent **STOP**.
5. **Done — the honest handoff:**
   ```
   ✓ Characterization complete · N runs logged to SD.
   Next: copy the SD card to your PC, run the analysis to
   generate oven_cal.h, then reflash both boards.   [ Done ]
   ```

Reached from Home → Calibrate, or from the drift advisory on the Run Summary (§16).
If the on-device→PC→reflash round-trip ever feels too heavy, the parked
**NVS-override** (§6) is how calibration could be applied on-device without a reflash
— not the baseline.

### Code architecture (per the skill)
MVVM via LVGL Observer/Subject: an **editor view model** owns the working profile +
`lv_subject_t` state and exposes intent methods (setPhaseTemp, addSegment, save…);
views only build widget trees and bind. Domain logic (template seeding, validation,
compile-to-segments) lives in `lib/app_logic/` with **no `lv_` calls** → host-tested
in `native_ui`/`native_logic`. Screens create-on-demand, state in subjects, delete
on leave (no PSRAM to hoard screens).
