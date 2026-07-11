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
- No cloud/app; WiFi is **local-only** convenience (data download + OTA both boards,
  §21) — never required for a run.
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
- **Firmware-update control lines (for OTA, §21):** two extra wires from the CYD to
  the controller's **BOOT0 + NRST** (STM32) / **GPIO0 + EN** (bench ESP32) let the CYD
  drop the controller into its ROM bootloader and reflash it over the UART. Budget
  them into the interconnect connector + controller PCB — without them, controller OTA
  needs a manual BOOT jumper.
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
- **Mode-dependent temp cap (two layers — DECIDED):** every run declares a `mode`
  (§9), and each mode is temperature-capped by **two** limits; the lower always wins:
  1. **Absolute per-mode hard-max — firmware constant in the *controller*.** The
     untrusted-proof backstop: it lives in the controller's compiled-in constants, so
     even a buggy/malicious CYD can't push a run past it. Reflow's sits near the
     element/SSR/thermal-fuse limit; cure/UV's is a conservative fixed ceiling.
     (Exact values TBD, §10.)
  2. **User per-mode max-temp *setting* — device settings, editable on the CYD.**
     Defaults **UV = 100 °C, reflow = 500 °C**. A profile in that mode **cannot be
     authored above its mode's setting** (editor stepper ceiling, §12) and the CYD
     validates before upload. The setting is adjustable only **within** the firmware
     absolute hard-max (layer 1 bounds its range) — never above it.

  Effective cap = `min(absolute_hard_max[mode], userMax[mode], recipe values)`; a
  recipe can only *tighten*. This **supersedes the old fixed 100 °C cure constant** —
  100 °C is now merely the *default* of the UV setting, not a hard-coded value.
  - **Safety weakening (flagged):** making the UV cap a user setting removes the old
    "UV can never exceed 100 °C" guarantee — a user can now raise it. What remains is
    the **UV absolute hard-max** (layer 1) bounding how high the setting goes, plus the
    independent high-limit. Keep the UV absolute hard-max conservative. As before, the
    L0 hardware cutoff sits at the *reflow* level, so a cure/UV over-temp is held by
    **firmware clamp + high-limit, not hardware** — acceptable at these temps but noted.
  - **Enforcement (DECIDED):** the user setting is enforced **CYD-side** (editor +
    pre-send validation); the controller enforces its **absolute hard-max** (clamp +
    NAK, §9). The user cap is a convenience/policy tightening, **not** the untrusted-
    proof limit — that stays layer 1. *Open (§10):* optionally also send the user cap to
    the controller for defense-in-depth.
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
| `conv_fan` (seg: on/off or duty; phase: Auto/On/Off) | convection during heat (uniformity) | optional |
| `cool_fan` (seg: on/off; phase: Auto/On/Off) | cooldown boost (donor's magnetron cooling fan) | optional |
| `uv` (on/off or duty) | off | on for the cure |
| `motor` (on/off) | unused | turntable — even-exposure under directional UV (§6) |

The controller sequences segments, PID-tracks `heater_setpoint_C` via the
zero-cross SSR, sets UV/fans/motor directly, and streams telemetry + progress
(current segment, elapsed, measured temp). Both modes ride the same executor;
"reflow vs cure" is just which channels a profile populates. The recipe also carries
a `mode` tag used *only* by the safety supervisor to pick the per-mode temp cap
(the controller's absolute per-mode hard-max; the user-set per-mode max lives on the
CYD — §4) — the executor itself stays mode-agnostic.

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
Per-phase channel settings (`conv_fan`/`cool_fan`/`uv`/`motor`) ride along. This is
why the whole projected timeline + ETA are computable up front (§15).

**Fan `Auto` mode (DECIDED — default for both fans).** Per phase, `conv_fan` and
`cool_fan` are **tri-state {`Auto` (default), `On`, `Off`}** (not plain on/off); `uv` and
`motor` stay explicit on/off. **`Auto` is resolved on the CYD at recipe-compile time**
from `oven_cal.h`: for each segment the CYD picks the fan state **needed to hit the
phase's ramp rate and hold the target** — turn `conv_fan` on when the requested heat ramp
(or hold uniformity) can't be met without it; turn `cool_fan` on when the requested cool
ramp is faster than passive cooling (§6 heat/cool-rate envelopes). This keeps the
architecture intact: the **stored profile carries the `Auto` intent** (so it re-resolves
if calibration changes), the **compiled `Recipe` carries the resolved on/off** (§9), and
the **controller stays a generic executor** — no fan policy in the safety MCU.

- **Fan-conditioned calibration (DECIDED):** the calibration fit **always produces** rate
  envelopes modelled **with each fan on vs off** (`heatRate(T, conv_fan)` /
  `coolRate(T, cool_fan)`) — a standard part of `oven_cal.h`, so `Auto` decides against
  real fan-on-vs-off rates. The characterization runs supply the data by randomizing
  per-phase fan state (below). See §6.
- **Pre-first-calibration fallback only:** before any calibration exists, `Auto` uses a
  simple heuristic — `conv_fan` on while heating, `cool_fan` on while cooling — flagged
  like the idealized preview (§12). Once calibrated, the fan-conditioned envelopes govern.
- **Reactivity:** resolution is compile-time-static (matches the up-front timeline/ETA,
  §15). A live/reactive controller-side variant is a parked enhancement — it would move
  fan policy into the controller, which we're deliberately avoiding for now (§10).

**Cure hold as "UV exposure per surface" (DECIDED).** In **cure** mode a phase's **hold**
is authored not as raw seconds but as the quantity the user actually cares about — **UV
exposure time per surface** — and the hold seconds `y` are **computed** from it. Because
the UV is a directional side beam and the part rotates on the turntable (§6), a surface
sits in the beam only a fraction of each rotation, so:
`y = exposure_per_surface / beamCoverage`, where **`beamCoverage`** (effective fraction of
a rotation a surface spends in the beam) is a **calibrated constant in `oven_cal.h`** (§6).
This mirrors the `RAMP_ASAP`→estimated-time pattern: the CYD computes `y` at compile time,
the segment still carries plain seconds, and the controller stays generic.
- **RPM-independent total:** over many rotations per-surface exposure is `y × beamCoverage`
  regardless of RPM — so `beamCoverage` is the conversion factor. RPM only needs to be
  **high enough that the hold spans many whole rotations** for evenness (§6).
- **Applies when UV + turntable are on.** With the turntable **off** (or pre-calibration,
  no `beamCoverage`) the cure hold falls back to **plain seconds** — the facing surface
  just gets the full time. **Reflow** is unaffected (its hold stays raw seconds).
- **Caveat:** "per surface" means the **azimuthal** side surfaces that rotate through the
  beam; top/bottom faces see the vertical beam differently — flip the part for those.
  `beamCoverage` is an *effective, empirically-characterized* factor (part size/position
  dependent), not a clean geometric constant (§10).

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
    (`lib/calibration/oven_cal.h` — `{a,b,τ}` + fan-conditioned heat/cool-rate envelopes
    + hard-max + turntable RPM + UV `beamCoverage`)
    that is **compiled into *both* firmwares.** One source → both binaries identical
    by construction (same single-source pattern as the shared `.proto`); fits the
    matched-pair invariant (§9). Supersedes the earlier "controller NVS" idea.
    - Tradeoff: **recalibration = rebuild + re-flash both boards** — now an **OTA
      bundle pushed over WiFi via the CYD** (§21), not a USB reflash, but still no
      *live* on-device recompute. Fine for a single bespoke unit. An optional NVS
      override (firmware defaults, NVS supersedes) could be added later if re-tuning
      gets frequent — not now.
    - Calibration also yields **max heat/cool-rate envelopes** (temperature-
      dependent — heating faster when cold, cooling passive/one-sided), used by the
      feasibility-aware curve preview (§12). Rate-limit + lag math is **shared
      `lib/` logic** reused by the CYD preview and the controller feedforward.
    - **Fan-conditioned envelopes (DECIDED — for fan `Auto`, §5):** the calibration fit
      **always produces** rate envelopes modelled **with each fan on vs off** —
      `heatRate(T, conv_fan)` and `coolRate(T, cool_fan)` — so the CYD can decide whether
      a fan is *needed* to hit a phase's rate. They're a standard part of the `oven_cal.h`
      deliverable, not optional. The characterization runs already supply the data by
      randomizing per-phase fan state (§5). The heuristic (§5) is only the pre-first-
      calibration fallback, never the steady state.
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
- **UV LED array (directional, side-mounted):** 405 nm via MOSFET (separate from
  heater), mounted in the **side of the chamber** — at the old microwave
  antenna/waveguide port. It's a **directional beam from one side**, *not* an
  all-around source, so a stationary part cures unevenly. The **turntable** is what
  gives even all-around exposure (see below) — the two are a functional pair for cure.
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
- **Turntable motor (cure — even-exposure mechanism, on/off DECIDED):** the donor's
  relay-switched low-RPM **synchronous AC** turntable motor — reuse as-is. It's
  **on/off only** (fixed native RPM; a synchronous AC motor's speed is locked to line
  frequency + gearbox, so it can't be varied by voltage/PWM — variable rate would mean
  swapping the motor, rejected as marginal for cure). Because the UV is a **directional
  side beam** (above), rotation is how every surface of the part passes through the
  beam — so the turntable is the primary **even-exposure** mechanism, not decoration.
  - **Exposure model (→ cure hold abstraction, §5/§12):** the meaningful cure quantity is
    **per-surface UV exposure time**. Over a hold `y` with UV + turntable on, per-surface
    exposure ≈ `y × beamCoverage`, where **`beamCoverage`** = effective fraction of a
    rotation a surface spends in the directional beam. So a cure phase is authored as
    *exposure per surface* and the CYD computes `y = exposure / beamCoverage`.
    - `beamCoverage` and the **turntable RPM** are **calibrated constants in `oven_cal.h`**.
      `beamCoverage` sets the dose↔time conversion (and the total is **RPM-independent**);
      **RPM** must merely be high enough that a hold spans many whole rotations for
      evenness — measured in bring-up/calibration.
    - `beamCoverage` is an **effective, empirically-characterized** factor (part
      size/position dependent), not a clean geometric constant — characterize it (§10).
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

**Separate per-mode libraries (DECIDED):** cure and reflow profiles live in
**independent stores** — separate LittleFS directories (e.g. `/profiles/cure/`,
`/profiles/reflow/`) each with its own stock seed set — and are **never mixed**. A run
can therefore only ever load a same-mode profile, so the §4 mode cap and §12 template
always match. Browsing/CRUD is in §23.

**Authoring (DECIDED): on-device editing + PC authoring.** Profiles can be
created/edited on the CYD touchscreen and saved to LittleFS directly. PC authoring
stays useful for seeding versioned defaults and bulk edits. This makes the CYD UI
own a profile editor (§ editing UI below).

**Transfer path (for PC-authored profiles):** profiles live in flash, not on the SD
card (so a run never depends on a card), so two mechanisms, not mutually exclusive:
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

**Device settings (DECIDED):** user preferences — units, per-mode **max-temp caps**
(UV/reflow, §4), sleep/brightness constants, WiFi — persist on the CYD (LittleFS or
NVS), separate from the profile library. Firmware ships the **defaults** (UV max
100 °C, reflow max 500 °C, §4); the Settings screen (§24) edits them, always within the
firmware absolute hard-max bounds. These are CYD-side policy — the controller's absolute
per-mode hard-max still governs independently (§4).

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
- **Retrieval (DECIDED):** the **SD stays permanently inserted**; logged files are
  pulled **over WiFi from the CYD's HTTP server** (§21), not by removing the card.
  Physically pulling the card is only an offline fallback.
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
6. **Connectivity services (§21):** WiFi data-download server + OTA (CYD self-update
   and controller-through-CYD reflash). Last, because it needs the link + calibration
   pipeline to exist first (data to serve, a matched pair to keep in sync) and the
   deployment board's BOOT/reset control lines wired.

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
  — `convFan`/`coolFan` are **resolved on/off** (the phase-level fan `Auto` is already
  applied CYD-side at compile time, §5); no `Auto` value crosses the wire.
  → upload the whole generic multi-channel recipe in one message (§5). `mode`
  (CURE/REFLOW) selects the controller's per-mode **absolute hard-max** (§4) — the
  executor stays generic; `mode` is used only by the safety supervisor.
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

### Firmware transport is out-of-band (note)
OTA (§21) does **not** ride this protocol. To reflash the controller the CYD tears the
TinyFrame link down and speaks the controller's **native ROM bootloader** protocol over
the same UART (STM32 system-memory loader / ESP32 serial loader), then re-establishes
the link and re-runs the `Hello` schema-hash handshake. That handshake gate is exactly
what catches a partially-applied bundle (stale ↔ new) and holds the system in safe
state until both boards match (§21).

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
  advanced-mode reorder UX, and building it in the sim. (Numeric entry designed: shared
  value-stepper §24 + keypad §26.)
- **Profile transfer:** `data/` + `uploadfs` baseline, and/or a serial/WiFi
  on-demand push? (§7)
- **Thermocouples:** exact fixed-channel count; **front-end IC** (MAX31855 vs
  MAX31856 vs analog-amp+ADC); whether the `{a,b,τ}` fit runs on-device or on a PC.
  (Estimator model, PID source, NVS storage now decided — §6.)
- **`.proto` + deps:** finalize the schema fields; add `nanopb` + `TinyFrame` to
  both PlatformIO envs and wire up the protobuf codegen step. (§9 — contract is
  designed; this is the mechanical follow-through.)
- **Controller field-update = OTA-through-CYD (DECIDED, §21):** the enclosed STM32 has
  no WiFi and no reachable USB, so CYD-driven bootloader reflash over the UART is the
  *primary* update path, not a fallback. Open: the CYD's embedded STM32/ESP32
  **bootloader-client** implementation, and verifying the **BOOT0/NRST control-line**
  wiring (§2) on the PCB.
- **Connectivity / OTA (§21, §25, §27):** WiFi provisioning path — **on-device join**
  (§27 setup flow) vs compile-time **`include/secrets.h`** (view goes status-only); OTA
  image **signing/authentication** on a mains appliance (local-net single-user — how much
  is enough?); CYD **partition layout** — does the app fit **twice** (A/B slots) alongside
  LittleFS in 4 MB? (measure once built; the controller image stages on **SD**, off the
  internal-flash budget, §25; fallback = 8/16 MB module or single-slot+recovery) + rollback
  behavior; the HTTP data-download endpoint — **auth** (open server on the LAN?), **mDNS
  `oven.local`**, **QR**, and on-device **delete-logs granularity** (§27); confirm the
  controller-first **flash sequencing + rollback** keeps the pair matched under any
  half-applied failure.
- **OTA bundle (§25):** the bundle **format** (both images + manifest) and **source** —
  fetched from a configured URL vs uploaded to the CYD's HTTP endpoint; the native
  **bootloader-client** implementation (STM32 system loader / ESP32 serial loader) and the
  BOOT/reset drive (§2); the recovery-UX for a failed controller flash.
- **Controller test lane** in the three-tier native/embedded setup.
- **UV cure specifics:** **characterize `beamCoverage`** (effective beam fraction) +
  **measure turntable RPM** for the exposure→hold-time conversion (§5/§6); UV as on/off or
  PWM duty; door-interlock wiring for the UV enclosure. (Decided: turntable on/off
  even-exposure under the directional side UV; cure hold authored as UV-exposure-per-
  surface → computed hold time, §5/§6/§12.)
- **Temp-cap limits (§4):** per-mode **user max-temp settings** now exist (defaults
  **UV 100 °C / reflow 500 °C**, editable in device settings). Still TBD: the per-mode
  **firmware absolute hard-max** constants (the untrusted-proof backstop) — reflow near
  the element/SSR/thermal-fuse limit (likely *below* the 500 °C default, so it governs
  in practice) and a conservative UV ceiling that bounds how high the UV setting may be
  raised. Open: whether to also enforce the user cap **controller-side** for defense-
  in-depth.
- **Deviation/drift thresholds:** live-cue band (§15) and end-of-run
  calibration-drift trigger (§16) — sustained-time N + RMSE/max thresholds; firmware
  constants, tune against real runs.
- **Fault overlay (§22):** finalize the `faultCode` enum in the shared `.proto` + the
  CYD's code→plain-language table; the buzzer pattern + RGB-LED behavior for
  annunciation (and whether ack silences vs waits for condition-clear).
- **Settings (§24):** which thresholds are user-exposed vs firmware constants (confirm
  the split); whether raising a temp cap needs more than the amber caution; manual
  brightness-bias range; global "restore defaults" scope.
- **Touch-target sizing:** the shared value-stepper editor (§24) now backs every numeric
  field incl. the §12 phase editor (resolved). Remaining check: all Settings **toggle
  rows** (units, auto-brightness, WiFi) render as full-width ≥56–67 px rows, and every
  `‹ Back`/prev-next chevron has a ≥56 px hit area despite being drawn small.
- **Profile library (§23):** ▲/▼ selection-highlight + auto-scroll (vs optional
  flick-scroll); rows-visible count; per-mode **restore-stock-profiles** action in
  Settings; default sort order;
  duplicate-naming scheme; where the Home **mode chooser** (Cure/Reflow) sits before the
  library. Also: `Save as…` in Setup persisting a tweaked working copy (§19).
- **Sleep constants (§17):** idle timeout (~1–2 min) and the safe-touch temperature
  below which sleep is allowed while cooling.
- **Auto-brightness tuning (§18):** LDR→backlight curve/LUT, min-floor/max-ceiling,
  filter/ramp time-constants — tune on real glass.
- **Cure resume (§15):** paused-state timeout before the CYD discards the remainder.
  (Resume gesture decided: press-and-hold, §15/§19.)
- **Calibration presets (§20):** the Quick/Standard/Thorough scope definitions
  (`n`/`m`/coverage/time).
- **Donor reuse (Toshiba ML2-STC13SAIT):** characterize the **humidity sensor's**
  analog interface (cure-mode only); decide **relay-board reuse** for on/off loads
  vs new drivers; decide whether to **reuse the donor SMPS** (5 V/12 V) for the
  controller + relay coils vs a new PSU (§2 power topology); whether to log the
  donor **NTC** as a bonus input. (§6)
- **Data logging + calibration pipeline:** the PC-side protobuf→CSV/Parquet decoder
  tool; the analysis that emits the generated `lib/calibration/oven_cal.h`
  (`{a,b,τ}` + fan-conditioned heat/cool-rate envelopes + turntable RPM + UV
  `beamCoverage`) compiled into both firmwares (§6);
  wall-clock timestamps without an RTC/WiFi (run-relative + run-id may suffice);
  SD-write buffering; verify the CYD's SD↔touch SPI coexistence on this unit. (§7)
- **Random-profile generator:** ranges/`n`/`m` defaults; pure-random vs structured
  sampling (e.g. Latin-hypercube) for better coverage. (§5)
- **Fan `Auto` resolution (§5/§6):** the exact decision rule (rate/target margins for
  turning each fan on). (Fan-conditioned envelopes are now a committed calibration
  deliverable — §6; the pre-first-calibration heuristic is the only fallback.) Whether a
  live/reactive controller-side variant is ever worth moving fan policy into the
  controller stays parked.

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

The editor edits **any profile buffer** — a **saved library profile** (Profiles → Edit,
§23) or a **run's ephemeral working copy** (Setup → Edit, §19). Same UI; only the save
target differs (a library file vs discarded when the run ends). Editing a **stock**
(read-only) profile becomes a **Save-as** so the factory reference survives (§23).

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
**Phase editor** — one phase as a **field list** (highlight + `Open`, like §23/§24). No
cramped inline steppers and no three-toggles-on-a-line: each numeric field opens the
shared **value-stepper editor** (~96 px −/+, §24), and each channel is a **full-width
toggle row**.
```
┌──────────────────────────────────────┐
│ ‹ Back            Soak (2/4)          │  header: back + which phase
├──────────────────────────────────────┤
│  Target                    165 °C   › │  ← selected; Open → value-stepper (§24)
│  Ramp                       MAX     › │  (Ramp at min = MAX / ASAP)
│  Hold                        90 s   › │
│  Conv fan               [ AUTO ] (on) │  fans: tri-state Auto/On/Off; (…) = what
│  Cool fan               [ AUTO ] (off)│  Auto resolved from calibration (§5/§6)
│  UV                          [ OFF ]  │  UV/motor stay On/Off
├───────────────┬───────────┬──────────┤
│      ▲        │     ▼     │  Open ›   │  move highlight · edit/flip selected
└───────────────┴───────────┴──────────┘
```
- **▲/▼** move the field highlight (auto-scroll — 6 fields, ~4–5 visible); **`Open`**
  acts on it: a numeric field → the value-stepper editor (§24); `uv` → flips on/off; a
  **fan** → cycles **Auto → On → Off**. On `Auto`, the row shows the resolved state in
  parentheses (e.g. `(on)`) so you see what calibration chose (§5). Default = `Auto`.
  All real touch targets are the three ~78 px footer buttons.
- Edits apply to the **working buffer** immediately; the **Overview's `Save ✓`** commits
  the whole profile, and leaving the editor without Save discards — so no per-phase
  Cancel/Done control is needed.
- **Move between phases** from the Overview tiles (tap another tile), keeping this screen
  a single-job field editor.

### Design-rule compliance
- **Numeric:** each field opens the shared **value-stepper editor** (~96 px −/+,
  min/max disabled-at-limit, tap-value→**constrained keypad** §26, §24) — never inline
  mini-steppers. Channel settings are **full-width toggle rows**. No free text except
  the profile **name** (on-screen keyboard, large keys, rare — on save-as).
- **Safety-limit clamp:** stepper ceilings = the **mode's max-temp *setting*** (device
  settings — default **UV 100 °C / reflow 500 °C**, itself bounded by the mode's
  firmware absolute hard-max, §4). UI prevents authoring an over-limit value; the
  controller still clamps/NAKs against its absolute hard-max as backstop (§9).
- **Validation, two tiers:**
  - *Hard-invalid* (over hard-max temp; peak ≤ soak ≤ preheat; non-monotonic times)
    → **red + word**, **blocks Save**.
  - *Physically-optimistic* — a ramp faster than the oven's calibrated max heat/cool
    rate → **amber + word**, draws the achievable curve, **allows Save** (the oven
    does its best; the user should see it but isn't blocked). (DECIDED.)
- **Idle-only:** the editor is unreachable during a run → no STOP button here (STOP
  lives on the run/monitor screens per the rules).
- Cure mode reuses this structure with one change: a phase's **Hold** field is authored
  as **UV exposure / surface** (not raw seconds), and the derived hold time is shown
  read-only — computed `= exposure / beamCoverage` from calibration (§5/§6). Falls back to
  plain seconds if the turntable is off or the oven is uncalibrated.

### Feasibility-aware curve preview (DECIDED)
The preview is **not** a naive plot of the entered setpoints — it incorporates the
oven's **calibrated capability**:
- **Requested** trajectory drawn as a ghost/dashed line (what you entered).
- **Achievable** trajectory drawn solid — the requested line **rate-limited** by the
  calibrated **heat/cool-rate envelopes** (temperature-dependent; cooling is the
  binding constraint since the heater is one-sided). For a phase on fan **`Auto`** the
  preview uses the **fan-resolved** envelope (§5) — i.e. the rate Auto's chosen fan state
  delivers — and if even that can't meet the request the phase is flagged amber. Where
  they diverge, the phase is flagged (amber, see validation).
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
├─ Profiles ─► (Cure | Reflow) ─► that mode's Profile library (§23) ─► Editor (§12)
├─ Calibrate ─► Calibration workflow ─► characterization run
└─ Settings (§24) ─► display & units / temp limits (§4) / sleep / WiFi
                / data & firmware (download · OTA, §21) / profiles / about / advanced
```

Setup (§19) starts **empty** and **Load**s a profile as an editable per-run template —
opening the mode's Profile library in **pick mode** (§23), then the editor on the
**working copy** (§12). Cure and reflow have **separate** libraries (§23).

### Global chrome (every screen)
- **Header:** screen title + **machine-state badge** (IDLE / HEATING / HOT / CURING
  / FAULT) + **link indicator**. The indicator pairs a **glyph + word** — `✓ Link` /
  `✗ No link` (schema mismatch = `✗ Schema`) — **never a bare colour dot** (a green/red
  dot alone fails the CVD rule; ~8% of males can't distinguish it).
- **Footer:** Back (left) + **STOP** (large red, right) — present + armed on every
  *running* screen; never blocked by UI work (machine work is off the LVGL loop).
- **Always visible:** machine state + link status (operator must never be unsure
  whether heat/UV is on, or whether the controller is responding).

### Cross-cutting overlays / states
- **Fault / alarm:** modal red overlay on controller `Fault` (§9); outputs already
  safe; shows code + plain-language cause; [Acknowledge]. **Designed in §22.**
- **Link lost / schema mismatch:** persistent header warning; **Start disabled** —
  no run may begin without a healthy link (safety). Mirrors the §9 gate.
- **Hazardous start confirmation** sits between Setup and Run (arm-then-start /
  press-and-hold; safe option default; red reserved for the hazardous verb).

## 14. Home / Status hub screen

Root of the hub-and-spoke; **sets the visual language** for every other screen.
Primary job: pick what to do, while making the machine's safety state unmissable.

```
┌──────────────────────────────────────┐
│ Oven Controller               ✓ Link  │  header: name + link (glyph+word, not dot)
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
- **Link lost / schema mismatch:** header indicator flips to `✗ No link` / `✗ Schema`
  (glyph+word + red, not colour alone) + body banner `⚠ Controller not responding`;
  **mode buttons disabled** — no run flow without a healthy link (mirrors §9).

### Visual language established for all screens
- Header = title + machine-state badge + link glyph+word. **The root hub is the sole
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
│ Reflow: LF-245        ⚠ HEATING   ✓   │  profile + state badge + link
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
- Fault → the modal alarm overlay (§22).

### Cure variant
Same skeleton, but: the curve is gentle (→ 80 °C hold, capped by the UV max-temp
setting — default 100 °C, §4), the
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
  Close the door, then hold Resume.   [ Abort ]   [ ██ HOLD Resume ██ ]
  ```
  On **Resume** (**press-and-hold**, §19; enabled only when the door is closed) the CYD
  **generates a remainder profile** — an `RAMP_ASAP` re-heat to the current target +
  the remaining hold/phases + remaining UV dose — and **`Start`s it as a fresh run.**
  To the controller it's just a new profile.
  - **Why CYD-side:** controller stays stateless → robust (a controller reset
    mid-pause loses nothing; the CYD re-sends). Reuses the phase/segment model + the
    profile generator; the board's cool-down is handled for free by the ASAP re-heat
    ramp at the front of the remainder.
  - **Resume re-energizes UV** → **press-and-hold** (§19) + door-closed-gated (no
    auto-resume, eye safety) — same friction as any UV/heat start (DECIDED).
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
- **Inactivity timeout:** ~1–2 min default, configurable (Settings, §24).
- **Wake sources:**
  - **Any touch** — the wake-tap is **consumed** (lights the screen without also
    actuating the control beneath it).
  - **Door-open event** from the controller.
  - **Incoming fault/alarm** — never hide a fault behind a dark screen (§22).
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
- **Optional Settings:** auto on/off + a manual brightness bias (in Settings → Display,
  §24); the safety min-floor always applies.
- **Code:** ports `IAmbientLight` (read) + `IBacklight` (set); a pure, host-testable
  `AutoBrightness` class (filter + curve + hysteresis + clamp) in `lib/` — mirrors the
  HAL/MVVM pattern.
- **Opens (§10):** the curve/LUT + floor/ceiling + filter time-constants (tune
  on-glass).

## 19. Setup & hazardous-confirm screens

The safety gate: **Setup** (load a template + review, with readiness checks) →
**Confirm** (deliberate, specific, hard to trigger accidentally). Strictest application
of the ui-development rules.

### Setup — start empty, Load a profile as a template (DECIDED)
A run begins with an **empty working profile** (no phases). You **Load** a saved
profile (§23) to seed it, then optionally **modify it for this run** — edits apply to an
**ephemeral working copy**, never to the saved profile. This makes every run a potential
one-off (realizing the earlier "custom profiles on demand" decision) without polluting
the library.

**Empty (nothing loaded yet)** — Load is the one primary action, a large central button:
```
┌──────────────────────────────────────┐
│ ‹ Reflow Setup            ● IDLE  ✓   │  mode + machine state + link
├──────────────────────────────────────┤
│        No phases yet.                 │
│      ┌────────────────────┐           │
│      │   Load a profile   │           │  large central primary (~200×64 px)
│      └────────────────────┘           │
├──────────────────────────────────────┤
│ ‹ Back              (Start ▶ — off)   │  Start disabled until loaded + valid
└──────────────────────────────────────┘
```
**Loaded (working copy seeded from a profile)** — provenance is an info line; the actions
get a **full-width button row** (three ~106×56 px buttons), not a cramped half-cell:
```
┌──────────────────────────────────────┐
│ ‹ Reflow Setup            ● IDLE  ✓   │  mode + machine state + link
├──────────────────────────────────────┤
│ From: LF-245 *      Peak 245° · 6:10  │  provenance + key facts (info, * = edited)
│ °C 250│      __                       │  feasibility-aware preview (§12)
│    25 │_ /  _/  \_ ___                │
├───────────┬───────────┬──────────────┤
│   Load    │   Edit    │   Save as     │  full-width action buttons (~106×56 px)
├───────────┴───────────┴──────────────┤
│ ⚠ Door open — close to start          │  readiness line (conditional)
├──────────────────────────────────────┤
│ ‹ Back                  Start ▶       │  → Confirm (disabled if not ready)
└──────────────────────────────────────┘
```
- **`Load`** → opens the Profile library in **pick mode** (§23); choosing a profile
  **copies** it into the working buffer (a template, *not* a live reference to the saved
  file). Re-Loading replaces the buffer — confirm first if it has unsaved edits.
- **`Edit`** → opens the profile editor (§12) bound to the **working copy** — per-run
  tweaks (nudge the peak, stretch a soak) that live only for this run. Provenance shows
  as `From: LF-245` with a **`*` dirty marker** once edited.
- **`Save as…`** (optional) → persist the tweaked working copy to the library as a
  **new** profile (§12 name entry → §7 LittleFS) — how a good one-off becomes reusable.
  Never silently overwrites the source.
- **Manual build** (advanced): the §12 advanced add-phase path can populate an empty
  working profile with **no Load**, for a fully hand-built one-off. Load is the common
  path.
- **Readiness gating:** `Start` enabled only when the working profile is **non-empty and
  valid** (≥1 phase, passes §12 hard-validation) **and** the **link is healthy** (§13)
  **and** the **door is closed** (`doorOpen` telemetry, §9/§17). The hardware interlock
  enforces the door regardless; the UI just shouldn't offer an un-runnable Start and says
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
   Next: download the logs over WiFi (Settings → data), run the
   analysis to generate oven_cal.h, then OTA both boards.  [ Done ]
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

## 21. Connectivity — WiFi services (data download + OTA)

The CYD's ESP32 WiFi (dead weight for control) provides two **local-network
convenience services**, both **idle-only** and never required for a run (§1).

### SD stays resident; data served over WiFi (DECIDED)
- The **microSD stays permanently inserted** as the run/characterization datastore
  (§7) **and the OTA bundle staging area** (§25) — no card-shuffling to a PC.
- The CYD runs a small **HTTP server** exposing the logged length-delimited-protobuf
  files (list + fetch) so the offline analysis/ML pipeline (§7, §10) pulls data over
  the LAN. Read-only, local network, served **only from idle** (so the SD↔touch SPI
  contention noted in §7 is a non-issue). The physical SD-pull path is kept as an
  offline fallback. The **on-device Connectivity & data view** (§27) shows the URL/QR to
  reach it.

### OTA firmware update — both boards, one bundle, via the CYD (DECIDED)
Both firmwares update **together, over WiFi, through the CYD** — reinforcing the
**matched-pair invariant** (§9): they can't drift because they ship and apply as one
bundle.

- **Why through the CYD:** the deployment controller (STM32) has **no WiFi** and, once
  enclosed, **no reachable USB** (§10) — so the CYD is its *only* practical field-update
  path. The CYD OTAs itself over WiFi **and** acts as an **in-system programmer** for
  the controller over the existing UART link.
- **Two flashing mechanisms:**
  - **CYD self-update:** standard ESP32 OTA — A/B (`ota_0`/`ota_1`) partitions +
    rollback; the new image is applied on reboot, a bad/mismatched one rolls back.
  - **Controller update (through the UART):** the CYD drops the controller into its
    **native ROM bootloader** and writes flash over the same wires — **not** via the
    TinyFrame/protobuf protocol (§9), which is torn down for the duration (see §9 "out-
    of-band" note). STM32: system-memory UART bootloader (`0x7F` sync + XOR-checksummed
    commands); bench ESP32: the ROM serial loader (esptool/SLIP). The CYD firmware
    embeds the matching bootloader client.
- **Extra interconnect (HARDWARE IMPACT, §2):** bootloader entry needs the CYD to drive
  the controller's **BOOT0 + NRST** (STM32) / **GPIO0 + EN** (bench ESP32) — **two
  control lines beyond the two UART signals.** The controller PCB + interconnect
  connector must budget for them; without them, controller OTA falls back to a manual
  BOOT jumper.
- **Sequencing + fail-closed:** flash the **controller first** — the riskier, over-UART
  leg, done while the CYD is still on its known-good image — then re-handshake and
  **verify `Hello.schemaHash`** (§9); only then apply the **CYD self-update** and
  reboot. Any half-applied outcome is caught by the **schema-hash gate**: a stale ↔ new
  pair makes the controller refuse to leave safe state and the CYD shows the mismatch
  error (§9). OTA and the gate compose — a botched update fails **safe, not hot**.
- **Safety gating (mandatory):** OTA is a deliberate, confirmed action allowed **only
  from idle AND cool** — never during a run, never while HOT (§17). During the
  controller-flash window the controller sits in its bootloader, **not** running the
  safety supervisor, so the fail-safe **pull-downs keep heater/UV OFF** (§4 L2) and
  mains should be de-energized — this is exactly why outputs default OFF on reset.

### Build / topology impact
- These services live in the **production firmware**, gated by a runtime **Settings →
  WiFi** toggle — **distinct** from the compile-time `UI_DEV_TOOLS` dev-server
  (screenshot/touch injection, ui-development skill), which stays dev-only. WiFi
  credentials still come from git-ignored `include/secrets.h`; on-device provisioning
  is an open (§10).
- **Flash budget:** the CYD partition table must fit **LittleFS profiles + A/B OTA
  slots** within 4 MB — verify (§10).
- WiFi remains a **non-goal for operation** (§1): a run needs no network; these are
  convenience/maintenance services only.

## 22. Fault / alarm overlay

The one screen that appears **unbidden**, over any other screen (including a sleeping
one, §17). Primary job: tell the operator, unmissably, that the machine **has already
been forced to a safe state** and get a deliberate acknowledgment. **It is an alarm +
acknowledgment, not a control:** by the time it draws, the controller has *already*
cut heater + UV (§4 L1–L3, §9 `Fault`) — there is nothing left to "stop," which is why
it can safely be a plain modal with no STOP button.

### Trigger sources (two origins)
1. **Controller `Fault{ session, code }`** (§9) — the controller safed itself and
   reported why: over-temp, sensor fault, electronics over-temp abort (§6), watchdog
   event, etc.
2. **CYD-detected link loss *during a run*** — if the UART goes silent mid-run the CYD
   can't *receive* a `Fault`, so it self-raises the overlay from its own
   heartbeat-timeout. The safety invariant still holds: the controller safes on its
   own command-timeout (§9) even though it can't tell us — the overlay says so rather
   than pretending to confirm live state.

**Not this overlay:** an **door-open during a run** is an *expected* event (§15), not a
red alarm — reflow shows "Run aborted — door opened," cure shows the Paused overlay.
Soft warnings (enclosure first-threshold, §4; calibration drift, §16) use **inline
amber banners**, never this modal. Keeping the red fault overlay rare is what preserves
its force (design rule).

### Layout (modal, red — the danger screen)
```
┌──────────────────────────────────────┐
│  ⛔  F A U L T                        │  red banner: icon + the word FAULT
├──────────────────────────────────────┤
│  Chamber over-temperature             │  plain-language cause (largest text)
│                                       │
│  Heater & UV are OFF · run aborted    │  what the system already did (reassure)
│  Chamber  268 °C                      │  the relevant live reading (if any)
│                                       │
│  Code: OVERTEMP_CHAMBER               │  small — for logs/support
├───────────────────┬──────────────────┤
│    Details        │   Acknowledge     │  Details (optional) · big ack (right)
└───────────────────┴──────────────────┘
```
- **Red is used in full here** — this is *the* hazard screen, the one place the design
  rules reserve it for — always paired with the word **FAULT** + icon (never colour
  alone).
- **Acknowledge is a plain single tap**, not press-and-hold: dismissing an alarm is a
  *safe* action (the hazard is already mitigated), and the design rules reserve
  press-and-hold for *energizing* actions. Large (≥67 px), on the right.
- **Details** (optional, secondary) expands the raw `faultCode`, `ctrlMillis`, and the
  last telemetry vector — for the SD log / troubleshooting, not the primary path.

### Fault taxonomy (plain-language mapping)
The controller owns the `faultCode` enum in the shared `.proto` (§9); the CYD maps each
code to a title + guidance via a table in `lib/app_logic` (host-testable). Initial set:

| Code | Title (CYD) | Typical cause |
|------|-------------|---------------|
| `OVERTEMP_CHAMBER` | Chamber over-temperature | high-limit / setpoint clamp tripped (§4 L3) |
| `OVERTEMP_CASE` | Electronics over-temperature | on-PCB case sensor abort (§6) |
| `SENSOR_FAULT` | Temperature-sensor fault | thermocouple open/short (MAX31855 fault bit) |
| `LINK_LOST` | Lost communication | heartbeat timeout (CYD- or controller-detected) |
| `WATCHDOG` | Controller reset (watchdog) | supervisor/loop hang → reset (§4 L2, §11) |
| `INTERNAL` | Controller fault | catch-all / assertion |

- **Unknown code → still informative:** a code the CYD's table doesn't recognize shows
  `Fault <code> — oven safed to a safe state` rather than a blank — defensive even
  though the matched-pair invariant (§9) should keep the tables in sync.
- `LINK_LOST` wording is special (can't confirm live state): *"Lost communication with
  the controller. If a run was active it safes itself automatically (heartbeat
  timeout)."* — reassurance via the invariant, not a live readback.

### Behavior (DECIDED)
- **Latching — never auto-dismiss.** The overlay stays until an explicit Acknowledge,
  **even if the condition clears** (e.g. the link returns). A fault is a human-in-the-
  loop event; it must not vanish on its own.
- **Wakes the display** (§17): a fault arriving during sleep lights the screen *then*
  draws the overlay — never hide a fault behind a dark backlight.
- **Multi-modal annunciation** (operator may not be looking): pulse the CYD's on-board
  **RGB LED red** and sound the **buzzer** while the fault is unacknowledged; both stop
  on Acknowledge. (Pattern/volume TBD, §10.) These *reinforce* the screen, never
  replace it.
- **Higher-priority fault while shown:** update the overlay to the new cause and keep a
  `+N` count; don't stack modals.
- **On Acknowledge:** route to the **Run Summary (Fault outcome, §16)** if a run was
  active (so the aborted run still gets its record + drift/cause context), else to
  **Home**. If the fault was over-temp, the **HOT** state (§14) persists on Home and
  keeps suppressing sleep (§17) until the chamber cools.
- **Acknowledge is always allowed** — it dismisses the *alarm*, not the *hazard* (which
  is already handled). We deliberately do **not** gate it on the condition clearing;
  the persistent HOT / link-lost indicators carry any residual state forward.

### Design-rule compliance
- Modal blocks everything beneath — acceptable here because nothing beneath is
  actionable once safed (no STOP to preserve; the abort already happened).
- Big-text cause first; ≥67 px ack target; visible pressed-state < 100 ms; contrast
  ≥7:1 on the red (critical readout).
- Red reserved for exactly this class of event; warnings stay amber and inline.

### Code architecture (per the ui-development skill)
- A top-layer LVGL object (`lv_layer_top`) so it draws over *any* screen incl.
  sleep-wake, decoupled from the current screen's lifecycle.
- **MVVM:** a `FaultViewModel` owns `lv_subject_t` state (active flag, current code,
  count); the controller gateway (the loop-side marshal, per the skill) sets it from
  `Fault`/link-timeout — **no `lv_` calls off the UI task.** The `faultCode → {title,
  guidance, severity}` table lives in `lib/app_logic` with no `lv_` deps → unit-tested
  in `native_logic`; the view only binds + renders. Latching + ack-routing logic is
  host-testable state, not view code.

## 23. Profile library

Browse/manage saved profiles and **Load** one into a run. **Two independent, mode-scoped
libraries** (cure, reflow) — never mixed (§7): a reflow curve is meaningless in cure and
vice-versa, so separation both keeps each list short on the small screen and guarantees a
run can only ever load a **same-mode** profile (the §4 cap + §12 template always match).

### Two contexts, one screen pair
- **Manage** — from **Home → Profiles → (Cure | Reflow)** (§13): full CRUD.
- **Pick** — from **Setup → `Load`** (§19): choose a profile → it's **copied** into the
  run's working buffer (a template, not a live reference); returns to Setup. Locked to
  the run's mode.

### Screen 1 — library list (mode-scoped)
```
┌──────────────────────────────────────┐
│ ‹ Reflow profiles                 ⧉   │  header: fixed mode (no cross-mode tab)
├──────────────────────────────────────┤
│  LF-245        peak 245° · ~6:10      │  ← selected (highlighted) row
│  SAC305        peak 249° · ~6:30      │  single-line text rows (compact)
│  LF-lowpk 🔒   peak 230° · ~5:40      │  🔒 = stock (read-only)
│  MyBoard       peak 240° · ~6:00      │
├───────┬────────┬────────┬────────────┤
│ + New │   ▲    │   ▼    │   Open ›    │  create · move sel ▲/▼ · open selected
└───────┴────────┴────────┴────────────┘
```
- **Rows stay single-line text** (compact → ~4–5 visible); you don't press them
  directly. The real gloved touch targets are the **four footer buttons (~78 px each,
  ≥67 px)**.
- **`▲` / `▼`** move the **selection highlight** one row (auto-scrolling the list at the
  edges — no separate paging, no drag; mirrors the §12 advanced-list up/down). **`Open ›`**
  acts on the highlighted profile → the detail/actions screen. (A direct row tap may
  also select, a bare-finger convenience, but the buttons are the guaranteed path.)
- Header reads **"Reflow profiles"** (manage) or **"Load reflow profile"** (pick); the
  mode is **fixed** either way — you never see the other mode's profiles here.
- **`+ New`** → a fresh profile seeded from this mode's **default template** (§12) →
  editor. (Distinct from Setup's empty-then-Load flow, §19.)
- Row facts (peak, est duration) are computed from the compiled-in calibration (§15/§6).

### Screen 2 — profile detail / actions
```
┌──────────────────────────────────────┐
│ ‹ LF-245                     ⧉ Reflow │  back + name + mode badge
│ °C 250│      __                       │  read-only derived curve (§12 preview)
│    25 │_ /  _/  \_ ___                │
│       └──────────────── t             │
│ peak 245 °C · ~6:10 · 4 phases        │  key facts
├───────┬───────┬───────┬──────────────┤
│  Load │ Edit  │  Dup  │   Delete      │  actions (Delete/Edit gated for stock)
└───────┴───────┴───────┴──────────────┘
```
- **`Load`** (primary in pick context): copies this profile into Setup's working buffer
  and returns to Setup (§19). In manage context it's a shortcut — enters this mode's
  Setup with it pre-loaded.
- **`Edit`** → editor (§12) on the **saved** profile (a persistent edit — unlike
  Setup→Edit, which edits the ephemeral working copy). On a **stock** profile, Edit
  becomes **Save-as** (source preserved).
- **`Dup`** → copy within the same library as "`LF-245 copy`" — a user (editable,
  deletable) profile.
- **`Delete`** → **user profiles only**; stock 🔒 disables it. Simple **confirm dialog**
  (not press-and-hold — deleting a file isn't an *energizing* hazard; press-and-hold
  stays reserved for heat/UV, §19/§22).

### Stock vs. user profiles (DECIDED)
Seeded defaults (`data/` → `uploadfs`, §7) are **stock / read-only** per mode — Edit
Save-as's, Delete is disabled — so the factory references can't be lost. **Restore stock
profiles** lives in Settings (per mode, §24).

### Behavior & tie-ins
- **Reflects LittleFS live:** the list *is* the mode's store — profiles pushed over
  serial/WiFi (§7/§21) land in the right mode dir and appear automatically.
- **Empty state:** "No profiles — New to create one" (shouldn't happen with stock seeds).
- **Sort:** recently-used first, then alphabetical (default, §10).
- **Rename** is via the editor (§12 name entry), not a separate library action.

### Design-rule compliance
Rows are compact single-line text (not pressed directly); the interactive controls are
the **four footer buttons ≥67 px** (New · ▲ · ▼ · Open) that drive selection + actions —
glove-safe without shrinking the list. Mode badge is category colour paired with a
word/icon (never colour alone); pressed-state < 100 ms; Delete behind a confirm. One
primary job: choose a profile.

### Code architecture (per the ui-development skill)
- A per-mode **`ProfileStore`** in `lib/app_logic` over a **storage port** (LittleFS
  adapter on device, in-memory fake on host) — list/load/save/delete/duplicate, no `lv_`
  deps → host-tested in `native_logic`.
- A **`ProfileLibraryViewModel`** scoped to one mode owns `lv_subject_t` list/selection
  state; views bind + render. The detail screen reuses the **shared curve-preview logic**
  (§12) for its read-only chart. Create-on-demand, delete on leave (no PSRAM, §skill).

## 24. Settings

The rarely-visited hub for preferences + maintenance. Reached from Home → Settings (§14),
**idle-only** (never during or with a staged run — some settings, e.g. temp caps, must not
change under a live profile). A **categorized hub → sub-panels**, using the same
glove-safe **▲/▼-highlight + `Open`** list pattern as the profile library (§23) so no
screen relies on precise small-target taps.

### Settings hub
```
┌──────────────────────────────────────┐
│ ‹ Settings                        ✓   │  back + link indicator
├──────────────────────────────────────┤
│  Display & units                      │  ← selected (highlighted)
│  Temperature limits                   │
│  Sleep & wake                         │
│  Network (WiFi)                       │
│  Data & firmware                      │
│  Profiles                             │
│  About                                │
│  Advanced ▢                           │  master toggle (shows state)
├───────────────────┬──────────┬───────┤
│        ▲          │    ▼     │ Open ›│  move highlight · open panel
└───────────────────┴──────────┴───────┘
```

### Panels (controls per category)
Every control obeys the sizing rules: **numbers** → the shared value-stepper editor
(~96 px −/+); **booleans** → **full-width toggle rows** (≥56–67 px, the whole row is the
target, not an inline `[ON]`); **choices** → the ▲/▼-highlight + `Open` list. No inline
mini-controls.
- **Display & units** — temperature **units** (°C/°F toggle; applies everywhere: editor,
  run, about); **auto-brightness** on/off + a manual **brightness bias** (→ the shared
  value-stepper editor, below) (§18). The **min-brightness floor always applies** even at
  lowest bias (HOT/UV/fault must stay legible, §18) — not user-defeatable.
- **Temperature limits** — the per-mode **user max-temp caps** (§4): UV and reflow, each a
  stepper (below). The one safety-relevant panel.
- **Sleep & wake** — **idle timeout** (~1–2 min default, §17; → the shared value-stepper
  editor). The **never-sleep-during-a-run** and **stay-awake-while-HOT** rules are **not**
  user-disableable (§17) — shown as fixed, not toggles.
- **Network (WiFi)** — enable toggle; join / status + IP → the **Connectivity & data view**
  (§27). All WiFi services are **idle-only** (§21). SSID/password use a **large-key
  on-screen keyboard** (the necessary free-text exception, like the profile name §12 — not
  a numeric field). Provisioning UX is open (§10).
- **Data & firmware** — the **Connectivity & data view** (§27, log download over WiFi) and
  **Check for / apply firmware update** → launches the **OTA flow** (§25), which is gated
  **idle AND cool** and updates **both boards as a matched pair**.
- **Profiles** — **Restore stock profiles**, per mode (cure / reflow): re-seeds the
  read-only stock set from firmware defaults (§23), behind a confirm.
- **About** — read-only: CYD `fwVer`, controller `fwVer` + caps, the `schemaHash`
  matched-pair fingerprint (§9), active **calibration** params + date (§6), board id.
  The place to verify a matched-pair after an OTA.
- **Advanced ▢** — a **master toggle** (off by default, progressive disclosure per the
  skill) that unlocks advanced **profile editing** (add/remove/reorder segments, §12) and
  any diagnostic options. Distinct from the compile-time `UI_DEV_TOOLS` dev-server (§21) —
  this is a *runtime user* toggle, not that.

### Temperature-limits panel (safety-relevant — DECIDED)
Two caps, each edited on its **own** value-stepper screen (below) — **not** two cramped
inline steppers on one panel (the +/− wouldn't meet the touch-target floor at 320×240).
The panel is a 2-row list; `Open` edits the highlighted cap.
```
┌──────────────────────────────────────┐
│ ‹ Temperature limits              ✓   │
├──────────────────────────────────────┤
│  UV cure max              100 °C      │  ← selected (highlighted)
│  Reflow max               500 °C      │
├───────────────────┬──────────┬───────┤
│        ▲          │    ▼     │ Open ›│  move highlight · edit selected cap
└───────────────────┴──────────┴───────┘
```

### Value-stepper editor (shared, glove-sized — DECIDED)
Every single numeric setting (a temp cap, idle timeout, brightness bias) opens **this
one-value-per-screen editor** with **large −/+ buttons (~96 px ≈ 17 mm, in the design
guide's 15–20 mm gloved-industrial band)**. Rationale straight from the panel math
(5.6 px/mm): +/− are **primary controls → 67–84 px minimum**, and two side-by-side
steppers can't hit that on a 240 px-tall panel — so each value gets its own screen
(progressive disclosure, as the guide intends). This one editor is reused everywhere a
number is set.
```
┌──────────────────────────────────────┐
│ ‹ UV cure max                     ✓   │  header (what you're editing)
├──────────────────────────────────────┤
│ ┌────────┐              ┌────────┐   │
│ │        │    100 °C    │        │   │  big − · large value · big +
│ │   −    │ (tap value → │   +    │   │  (−/+ ≈ 96×96 px)
│ │        │    keypad)   │        │   │
│ └────────┘              └────────┘   │
│  Range 60–120 · default 100 °C        │  min/max + default (context)
│  ⚠ Above default — higher burn/fire   │  amber note, conditional (caps only)
│     risk; hardware fuse still governs │
├──────────────────────────────────────┤
│ ‹ Cancel                    Save ✓    │
└──────────────────────────────────────┘
```
- **Large −/+ (~96 px), disabled at min/max** (guide: disable, don't hide); value large +
  centered; **tap the value → constrained numeric keypad** (large keys, §26) for big jumps;
  **press-and-hold −/+ accelerates** (the guide's stepper pattern).
- For a **temp cap**, the −/+ ceiling is the mode's **firmware absolute hard-max** (§4
  layer 1): the value moves only *within* it, **never loosens past it**; the controller
  enforces the hard-max regardless (untrusted-CYD-proof, §4/§9).
- **Save is a plain button, not press-and-hold** — editing a cap starts no heat/UV (the
  §19 Confirm is the real energizing gate). But **raising a cap above default** shows the
  **amber caution** — a visible nudge without confirmation friction.
- On **Save**, any profile loaded in Setup is **re-validated** against the new cap (§12
  hard-validation) — lowering below a loaded profile's peak flags it.

### Persistence & behavior
- All settings persist on the CYD (LittleFS/NVS, §7), separate from the profile stores;
  firmware ships the **defaults** (units °C, UV cap 100 °C, reflow cap 500 °C, idle ~1–2
  min, auto-brightness on). "Restore defaults" (global) lives here too.
- Changes apply on the panel's **Save** (or immediately for toggles); idle-only entry means
  none of this races a run.

### Code architecture (per the ui-development skill)
- A typed **`SettingsStore`** in `lib/app_logic` over the storage port (LittleFS adapter /
  in-memory fake), with **validation baked in** — e.g. the shared value-stepper editor
  clamps a cap to the firmware hard-max — host-tested in `native_logic`. The value-stepper
  editor is a **reusable widget** (min/max/step/units/keypad), used by settings and the
  profile editor (§12) alike.
- Per-panel **view models** expose `lv_subject_t` settings; views bind + render. The temp
  caps publish to the subjects the profile editor (§12) reads for its stepper ceilings, so
  a changed cap tightens the editor with no extra wiring. Hub + panels create-on-demand,
  delete on leave (no PSRAM, §skill).

## 25. OTA firmware-update flow

A wizard that updates **both boards as one matched pair** over WiFi, launched from
**Settings → Data & firmware → Update firmware** (§24). Its defining property is the
**fail-closed staging** (§21): the controller is flashed **first**, while the CYD is still
on its known-good image, and the **schema-hash gate** (§9) catches any half-applied pair —
so a botched update ends **safe, not hot**. This is a maintenance flow, not a machine
process: it runs **only idle AND cool** and there is no heat/UV anywhere in it.

### Preconditions (the gate — all must hold to start)
- **Idle AND cool** — no run staged/active, chamber below the safe-touch threshold (§17).
  Blocked with the reason otherwise (`⚠ Let the oven cool before updating`).
- **Link healthy + schema OK now** (§9) and **WiFi connected** (§21).
- **A valid bundle is available** (below). Any failure → the flow won't arm; it says why.

### Update bundle
One artifact carrying **both images + a manifest**: `{ cydImage, ctrlImage, cydVer,
ctrlVer, schemaHash, hardwareId, per-image checksums }`. The `schemaHash` is the pair's
fingerprint (§9) — the two images are built together, so the bundle *is* the matched pair.
The CYD **validates before flashing anything**: checksums, `hardwareId` matches this unit,
and the manifest is well-formed. Bundle **source/signing** is an open (§10) — fetched from
a configured URL or uploaded to the CYD's HTTP endpoint (§21); signing/auth still TBD.

### Memory & staging (no image is ever held in RAM)
OTA is **stream-to-storage, never buffer-in-RAM** — the CYD's ESP32-WROOM has **no PSRAM**
and only ~300 KB SRAM (far less free with WiFi + LVGL up), nowhere near a firmware image,
so neither image is ever fully resident.
- **The bundle stages on the SD card** (always inserted — SD is the OTA staging area too,
  §7/§21). Gigabytes free, and it keeps **both images off the 4 MB internal flash**.
- **CYD self-update** streams SD → the inactive A/B slot (`ota_1`) in ~KB chunks while
  running from `ota_0`; the **controller image** streams SD → UART chunk-by-chunk, hashed
  on the fly — neither is fully buffered.
- So the **4 MB internal flash** only carries the **CYD app twice** (`ota_0`/`ota_1`) +
  bootloader/NVS/otadata + LittleFS (profiles + settings). Whether the app fits **twice**
  in 4 MB is the binding question (§10) — measure once built (~1.5 MB/slot ceiling with
  LittleFS alongside). Fallbacks if not: an **8/16 MB flash CYD variant**, or
  **single-slot + recovery** (giving up seamless A/B rollback).

### Staged sequence (fail-closed)
```
Preflight ─► Review/Confirm ─► [1] Flash controller (UART) ─► [2] Verify controller
          ─► [3] Flash CYD (self, A/B) ─► reboot ─► [4] Post-reboot result
```
1. **Flash controller (over UART).** The CYD drives **BOOT0+NRST / GPIO0+EN** (§2) to drop
   the controller into its **ROM bootloader** and writes + verifies flash via the native
   loader (§21) — *not* the TinyFrame protocol. The riskiest leg, done **while the CYD is
   still on its good image** so a failure here is fully recoverable.
2. **Verify controller.** Re-establish the TinyFrame link, exchange `Hello`, and **check
   `schemaHash` == the bundle's** (§9). Mismatch / no response → **abort before touching
   the CYD** (keep a working CYD; offer retry).
3. **Flash CYD (self).** Write the new image to the **inactive A/B slot** (`ota_1`), mark
   it to boot, then **reboot**. The old slot is untouched.
4. **Post-reboot result.** The new CYD boots, re-handshakes, and confirms the pair's
   `schemaHash`. Match → **commit** the new slot + show success. If the new image is bad
   (won't boot) → ESP32 **rolls back** to the old slot → old-CYD + new-controller =
   **schema mismatch** → the §9 gate **holds safe state** and shows the mismatch, prompting
   a re-run. Every partial outcome converges to *safe*.

### Screens
**Review / Confirm** — plain confirm (reflashing starts no heat/UV, so **not**
press-and-hold, §19/§22; the risk is interruption, not energy):
```
┌──────────────────────────────────────┐
│ ‹ Firmware update                 ✓   │
├──────────────────────────────────────┤
│  Display (CYD)   v1.3  →  v1.4        │  old → new, both boards
│  Controller      v0.8  →  v0.9        │
│  Schema  ✓ matched pair               │  bundle is a matched set (§9)
│                                       │
│  ~3 min · both boards restart.        │
│  ⚠ Keep powered — do NOT switch off.  │  interruption caution (not energy)
├───────────────────┬──────────────────┤
│     Cancel        │     Update        │  plain confirm; Cancel is the easy one
└───────────────────┴──────────────────┘
```
**Flashing** — long op → progress + percent + persistent "do not power off" (guide's
>10 s rule). **No Cancel once a write begins** (interrupting risks a bad image); Cancel is
offered only *before* stage 1:
```
┌──────────────────────────────────────┐
│  Updating firmware                    │
│                                       │
│  [1/2] Controller                     │  which stage
│  ▓▓▓▓▓▓▓▓▓▓▓░░░░░░░░░  57%            │  progress bar + percent
│                                       │
│  ⚠ Keep powered — do NOT switch off.  │  persistent, whole flow
│  Outputs are OFF.                     │  reassurance (pull-downs, §4 L2)
└──────────────────────────────────────┘
```
**Result** — success or a recovery state (below). Success is really the *next* normal boot
confirming the pair; the flow ends on a `✓ Updated — CYD v1.4 · controller v0.9 · schema OK`
card with **[ Done ]**.

### Failure & recovery (every partial state is recoverable)
| Failure point | State | Recovery |
|---|---|---|
| Bad/mismatched bundle | nothing flashed | reject up front; fix the bundle |
| Controller flash fails | CYD good, controller partial | **Retry** — the controller's **ROM bootloader is always reachable** via BOOT0 (§2), so it's **not bricked** |
| Post-flash verify mismatch | controller new-but-wrong, CYD untouched | abort before CYD; retry controller |
| CYD self-update / rollback | old CYD + new controller | §9 schema gate **holds safe** + shows mismatch → re-run OTA |
| Power loss mid-flash | controller partial and/or CYD on old A/B slot | on repower: ROM bootloader + A/B good-slot make it recoverable; CYD detects no-valid-controller / mismatch → offers re-flash |

The reassurance to surface throughout: **the controller ROM bootloader is immutable and
the CYD keeps a known-good A/B slot**, so there is no single step that bricks the unit.

### Safety & design-rule compliance
- **Idle+cool gate**, no heat/UV in the flow; during the controller-bootloader window the
  supervisor isn't running, so **fail-safe pull-downs keep heater/UV OFF** (§4 L2) — the
  flow shows "Outputs are OFF."
- **No STOP** (nothing is running; not a heat process) — the one screen class exempt from
  the persistent-STOP rule, like the root hub (§14).
- **Plain confirm, not press-and-hold** — reflashing isn't an energizing action; the
  "do not power off" warning addresses the real (interruption) risk. Press-and-hold stays
  reserved for heat/UV (§19/§22).
- Progress + percent + "keep powered" for the multi-minute op (>10 s rule); big-text
  stage/percent; buttons ≥67 px.

### Code architecture (per the ui-development skill)
- An **`OtaController`** in `lib/app_logic` drives the state machine (preflight → stages →
  verify → result) over ports: a **`BundleSource`** (validate/read images), a
  **`ControllerProgrammer`** (BOOT/reset + native-loader client — the STM32/ESP32
  bootloader protocol, host-fakeable), the **`ISerialTransport`** (§11) for the verify
  handshake, and the ESP32 **self-OTA** API. Staging + fail-closed logic is **host-tested**
  in `native_logic` with fakes (simulate flash-fail, verify-mismatch, rollback).
- An **`OtaViewModel`** exposes `lv_subject_t` progress/stage/result; the flashing work
  runs off the LVGL loop (gateway pattern, §skill) so the UI stays responsive. Screens
  create-on-demand.

## 26. On-screen numeric keypad

The **type-from-scratch / big-jump** partner to the value-stepper editor (§24). Reached by
**tapping the value** in the value-stepper editor (so it backs every numeric field — temp
caps, phase target/ramp/hold, exposure, timeouts, brightness bias, §12/§24). Steppers are
for nudging; the keypad is for large changes and fresh entry. It's a **shared modal**
configured per field with `{min, max, units}`.

### Layout math (why 5×2, not a phone 3×4)
A phone-style **3-column × 4-row** pad **doesn't fit** at 320×240: four key rows at the
56 px floor = 224 px, leaving nothing for a header + value display. So digits go in a
**wide 5-column × 2-row** block instead — that keeps keys **≥56 px on both axes** and still
leaves room for the value line and a control row.
```
┌──────────────────────────────────────┐
│ Target temp                        ✕  │  field name + Cancel (✕, ≥56 px hit area)
├──────────────────────────────────────┤
│   245 °C            min 60 · max 500   │  live value + units + range (always shown)
├──────┬──────┬──────┬──────┬───────────┤
│  1   │  2   │  3   │  4   │     5      │  digit row 1  (~64×60 px keys)
├──────┼──────┼──────┼──────┼───────────┤
│  6   │  7   │  8   │  9   │     0      │  digit row 2
├──────────┬──────────┬─────────────────┤
│    ⌫     │  Clear   │      OK ✓        │  ~106×54 px controls; OK widest (primary)
└──────────┴──────────┴─────────────────┘
```
- Digit keys ≈ **64×60 px** (320/5 wide, two rows in the remaining height); controls
  ≈ **106×54 px**. All clear the 56 px floor; OK is the widest/primary.
- **Integer-only (DECIDED — no `.` key).** Every user-entered numeric field is a whole
  number — temps (°C), times (s), timeout (min), brightness bias (%). The only fractional
  values in the design (ramp *rate* `−2.0/s`, live temps, calibration constants) are
  **derived/displayed, never typed** (§5/§6/§12). Dropping decimals removes the `.` key,
  decimal-position logic, and float-rounding entirely. **Escape hatch:** if a rate-based
  entry field is ever added, decimals become a small per-field extension — not built now.
- **No `+/−` key** — no field takes a negative (cool rates aren't entered directly, §5).
  Values shown as plain integers.

### Behavior (constrained, not validated-after — the guide's rule)
- **Range is enforced, not just checked.** Digits that would push the value **over `max`
  are blocked** (can't type an out-of-range number); **`OK` is disabled until the value is
  in `[min,max]`** (covers the incomplete-low case, e.g. still below `min`). The value goes
  **amber with the range reminder** while `OK` is disabled — you always see why.
- **`⌫`** deletes the last digit; **`Clear`** empties; both give instant (<100 ms) value
  feedback. Every key shows a pressed state immediately.
- **`✕` / Cancel** discards and returns to the stepper editor with the **old** value;
  **`OK`** commits the new value back to the field. The keypad never talks to the
  controller — it just edits a number.
- Opens **pre-loaded with the current value** and the field's units/range; sensible.

### Relationship to the value-stepper editor (§24)
Together they are the guide's "coarse + fine" numeric pattern: the **stepper** (±, ~96 px)
for small deliberate changes at a limit, the **keypad** for wide-range entry — sharing one
per-field config so `min/max/units` are identical whichever you use, and both
clamp to the same firmware bound (e.g. a temp cap's hard-max, §4).

### Code architecture (per the ui-development skill)
- A pure **`NumericEntry`** logic class in `lib/app_logic` (append-digit, backspace, clear,
  in-range test, over-max block) — **no `lv_`**, host-tested in `native_logic` (the
  range/clamp/edge rules are exactly the kind of logic to unit-test off-device).
- A reusable **keypad view** binds to it via `lv_subject_t` (typed value, valid flag);
  opened as a modal over the value-stepper editor, returning the committed value through a
  subject. One widget, configured per field — no per-screen keypads.

## 27. Connectivity & data view

Where you go to **get logged data onto a PC** and manage WiFi. Reached from Settings →
**Data & firmware** (the data half; the firmware half is OTA, §25) and Settings →
**Network** (§24). **Idle-only** — all WiFi services are (§21).

**Key framing:** the actual file browsing/downloading happens in a **PC (or phone)
browser** against the CYD's HTTP server (§21) — so this screen's job is to get you *to*
that server (**URL + QR**) and summarize what's on the SD, **not** to list every file with
download buttons (a poor use of 320×240 when the PC does it better). The header keeps the
global **controller-link** glyph (§13); WiFi status is a separate, clearly-labelled body
line so the two links aren't confused.

### Connected state (the primary view)
```
┌──────────────────────────────────────┐
│ ‹ Connectivity & data             ✓  │  back + controller-link glyph (§13)
├──────────────────────────────────────┤
│ WiFi ✓ MyShopNet        192.168.1.42  │  WiFi status: SSID + IP (own line)
├───────────────────────┬──────────────┤
│ Get your data — on a  │  ┌─────────┐  │  the one primary job
│ PC/phone browse to:   │  │ ░▓░▓ QR │  │  QR of the URL (scan to open)
│  oven.local           │  │ ▓░▓░    │  │  mDNS hostname (stable)
│  (192.168.1.42)       │  └─────────┘  │  IP fallback
├───────────────────────┴──────────────┤
│ Logs  37 runs · 12.4 MB · SD 61% free │  storage summary
├───────────┬──────────────────────────┤
│ WiFi setup│        Delete logs        │  join/change · delete (→ confirm)
└───────────┴──────────────────────────┘
```
- **Reach it two ways:** a stable **mDNS hostname `oven.local`** (survives DHCP address
  changes) with the raw **IP** as fallback, plus a **QR of the URL** (scan with a phone to
  open the file listing, or point a laptop camera). Big, legible URL — it's meant to be
  read and typed.
- **Storage summary**, not a file list: run count, total size, SD free. The per-file list +
  download live in the PC browser (server serves the length-delimited protobuf logs §7 →
  PC decoder → DuckDB/CSV/Parquet).

### Other states
- **WiFi off:** body shows `WiFi off` + a single **[ Turn on WiFi ]**; no URL/QR.
- **Connecting / no network:** `WiFi — connecting…` or `not connected`; **[ WiFi setup ]**
  prominent; no server URL until associated.
- **Connected but idle-gated:** the server is available **only while idle** (§21) — if a
  run were active the view isn't reachable anyway (idle-only entry).

### WiFi setup (join) sub-flow
`WiFi setup` → scan/list networks (the §23 ▲/▼-highlight + `Open` list) → pick SSID → enter
password on the **on-screen keyboard** (text entry — the one remaining minor UI piece, §10)
→ associate. **Provisioning path is still open (§10):** if on-device provisioning is
implemented this is the flow; if creds stay **compile-time in `include/secrets.h`** (§21),
this view is **status-only** and `WiFi setup` is hidden/greyed. The view is built to work
either way.

### Storage management
**Delete logs** frees SD space (the SD is permanent, §21, so on-device cleanup is useful).
It's **data loss, not a machine hazard** → a **plain confirm dialog** (not press-and-hold,
which stays reserved for heat/UV, §19/§22), and it sits visibly apart from the benign
actions. Per-file deletion stays on the PC; on-device is **delete-all** (granularity is a
minor open, §10).

### Design-rule compliance
- **Idle-only**; **no STOP** (nothing runs here — the maintenance-screen exemption, like
  the hub §14 and OTA §25). Buttons ≥67 px; **Delete** behind a confirm and isolated from
  benign controls. The **URL is a primary, large, high-contrast readout** (it's the datum
  the screen exists to convey). Colour only for state (WiFi ✓/✗ glyph+word, never a bare
  dot — §13).
- **Read-only + local:** the server only *serves* logs on the LAN; **auth is an open**
  (§10) — worth a one-line "anyone on your network can read the logs" note in the UI.

### Code architecture (per the ui-development skill)
- A **`ConnectivityViewModel`** exposes `lv_subject_t` state: WiFi phase
  (off/connecting/connected), SSID, IP + `oven.local`, the server URL, and the **log
  summary** (run count, bytes, SD-free). WiFi + HTTP server run **off the LVGL loop**
  (gateway pattern, §skill) and marshal events into the subjects.
- The **log summary** comes from scanning the SD log dir behind the storage port
  (`lib/app_logic`), host-testable (counting/sizing logic, no board). The **QR** is
  generated locally from the URL string (small encoder → canvas). The join flow reuses the
  §23 list + the on-screen keyboard.
