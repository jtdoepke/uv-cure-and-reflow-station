# Design — Oven Controller (UV Curing & PCB Reflow)

> Working design doc. Built interactively; sections marked **TBD** are open.
> Scope: whole-system architecture. Detailed subsystem docs (safety, control,
> hardware) will hang off this once the skeleton settles.

## 1. Goals & non-goals

### Goals

- One bench box, two jobs: **PCB reflow** (follow a solder-paste temp curve) and
  **UV resin curing** (gentle heat ~80 °C + UV, on a timer).
- Touchscreen HMI on the CYD where the oven keypad used to be.
- A library of named profiles: one per solder paste, one per resin —
  **editable on-device**, not just authored on a PC.
- Safe by construction: mains heat can't run away even if firmware/SSR fails.
- **Custom controller PCB for deployment**, built around a second
  **ESP32-WROOM-32E** (same target as the bench dev board; radio simply unused —
  §2).

### Non-goals (for now)

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
  safety enforcement. **One target (DECIDED — revised from the earlier STM32 plan):**
  - **ESP32-WROOM-32E, bench and deployment.** Bring-up runs on the dev board on
    hand; deployment puts the same module on the custom PCB. The radio is simply
    left unused — deleting it isn't worth a second embedded project: same 3.3 V
    logic and toolchain as the CYD, adequate timers (LEDC/MCPWM) for
    zero-cross/PWM, shared-SPI + per-chip CS for the thermocouple front-ends,
    hardware watchdogs, and a ROM serial loader (esptool/SLIP) the CYD needs a
    client for anyway (§21/§25). One toolchain, one HAL adapter, one bootloader
    client; "port to deployment" becomes "same firmware, new board."
  - *STM32 (G0/C0/F103 class) was the earlier pick and was set aside:* its
    advantages don't discriminate at N=1, and it drags in a second toolchain,
    `Stm32*` adapters, a second bootloader client (AN2606/8E1 quirks,
    `nBOOT_SEL` option-byte provisioning), and new-silicon safety re-verification.
    Revisit only if a concrete constraint (BOM, supply, certification) appears.
    *ATtiny was considered and set aside:* an 8-bit tiny is
    cramped for multi-SPI + framed protocol + PID + safety and weakens the shared
    host-tested-logic story (revisit only if the AVR toolchain is specifically
    wanted). The controller firmware is still written against a **HAL / port
    layer** (mirroring the CYD's `lib/display_port` `IDisplay`/`ITouch` pattern):
    safety + control + protocol logic stay MCU-agnostic and host-testable; only a
    thin per-target adapter (pins, SPI, PWM, timers, watchdog) changes. Board swap
    = new adapter, not a rewrite. Multiple PlatformIO envs select the target.

**Interconnect (DECIDED):**

- **Pin inventory (the hard constraint).** *Board-specific: the inventory below is the
  **2.8" ESP32-2432S028**'s, which was the only board when this was written. The default is
  now the **3.5" ESP32-3248S035**, whose free pins are **GPIO25/32** — free precisely because
  its touch shares the display's SPI bus, so the 2.8"'s bit-banged touch pins are spare, while
  its GPIO27 is the backlight. Per-board pin maps live in `include/cyd_board.h` + the
  `LGFX_*.hpp` headers; the "zero slack" conclusion below is about the 2.8" specifically, but
  the shape of the argument — a handful of free pins, hence a second MCU for the outputs —
  holds on both.*
  The 2.8" CYD exposes three plugs — **P5** (VIN, TX, RX, GND), **CN1** (3.3 V, GPIO27,
  GPIO22, GND), **P3** (GPIO21, GPIO22, GPIO33, GND). P3 contributes **zero free pins**:
  GPIO21 is the backlight PWM and GPIO33 the touch CS, and its
  GPIO22 duplicates CN1's. So the only free GPIOs are **CN1's 27/22 and P5's TX/RX
  (GPIO1/GPIO3)** — exactly the four the interconnect needs, which is why P5's
  TX/RX get used after all (as the rarely-driven OTA lines, below, not the link).
  *(Header names verified against the board during A8: the connector carrying the
  two free GPIOs is CN1, not P3 — this doc had the two labels swapped. The free-pin
  set, and therefore every conclusion below, is unchanged.)*
- **Comms:** a dedicated *second* hardware UART — **CYD GPIO27=TX / GPIO22=RX
  (CN1)** crossed to the controller's UART. 3.3 V both sides → no level shifting.
  Deliberately *not* P5/UART0, so the CYD's USB serial monitor + flashing stay
  free of contention. On the controller side the link **must land on UART0
  (GPIO1/3)** — the pins its ROM serial loader speaks (§21/§25); controller
  boot-ROM chatter on its TX0 arrives as garbage frames the CRC rejects.
  Protocol detail in §9.
- **Power:** a mains-derived **5 V PSU on the controller side** powers the
  controller and feeds the CYD via **P5 (VIN + GND)**. The CN1 signal cable carries
  **TX/RX + GND as a twisted triple** — the signal return must not detour through
  the power cable (that loop area next to a mains-switching SSR is how phantom
  CRC/heartbeat faults happen); P5's ground remains the power return. Route both
  cables away from the SSR/contactor. **Heavy loads (UV array, SSR/relay coils,
  motor) get their own PSU rails — never routed through the CYD.**
- **Firmware-update control lines (for OTA, §21):** the CYD drives the
  controller's **GPIO0 + EN** to drop it into the ROM serial loader and reflash it
  over the UART. These ride **P5's TX/RX**: **GPIO3 (RX0, quiet at boot) → EN**
  and **GPIO1 (TX0) → GPIO0/BOOT** — GPIO1's boot-ROM chatter can glitch only
  BOOT, which is harmless while EN isn't being toggled. **Hardening (DECIDED):**
  the CYD drives both lines **open-drain** and leaves them released (inert) at
  boot/crash; the controller PCB carries local **pull-ups on EN and GPIO0** plus a
  series-R/RC on EN, so a dead or rebooting CYD cannot hold the safety MCU in
  reset or in its bootloader. Known bench caveat: USB-flashing the CYD wiggles
  GPIO3 (CH340 TX), so the controller may reset repeatedly during a CYD bench
  flash — harmless (fail-safe defaults, §4), and field OTA never involves USB.
- **Bench exception — the controller's link moves to UART2 (DECIDED, backlog A8):**
  the link must land on UART0 in production (above), but on a dev board UART0 is
  *also* the USB-serial bridge's port, and a powered bridge idles its TX **driven
  high** — so it wins arbitration against the CYD's TX on the controller's RX0. You
  cannot have both boards on USB *and* the link up; a series resistor does not help,
  it only decides who loses. The `esp32dev_control_bench` env (`-D CONTROL_BENCH`)
  therefore puts the **controller's** link on **UART2 (GPIO16/17)** and keeps UART0
  as flash + monitor. Production `esp32dev_control` is unchanged — link on UART0,
  and consequently **no console at all**: a stray `Serial.printf` there would be
  bytes on the wire, which is also why TinyFrame's `TF_Error` is compiled out there
  (`TF_ERROR_QUIET`). The CYD needs no such flag; its link already lives on CN1's
  GPIO27/22, clear of its own USB. Cost: the bench pinout is not production's, so
  link-on-UART0 must be re-verified before §25 — §8 step 2's re-run of the step-1
  proof against real hardware is where that lands.
- **Build caveat:** with the PSU on VIN, don't dual-source 5 V (PSU + USB) while
  flashing the CYD — disconnect the PSU or rely on the board's USB diode. On the
  two-devkit bench the same rule bites in reverse: leave the boards' 5 V and 3.3 V
  rails **unconnected** (GND + the two signal lines only) and let each self-power
  from its own USB. Tying 3.3 V would parallel two LDO outputs — the higher one
  hogs, and since AMS1117 has no reverse-current protection, unplugging one board
  leaves it half-alive through the tie, which would quietly invalidate every
  reset-based step of the §8 step-1 proof.

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

**Revised 2026-07-17 — the CYD is a UI remote; the controller is the brain
(DECIDED).** The earlier split still stored the profile *library* and device
*settings* on the CYD (§7) and put WiFi/OTA/the data-server on the CYD (§21/§25/§27).
Continuing development showed the CYD has **no DRAM budget left** for that on top of
LVGL: the binding constraint is the static `dram0_0_seg` segment (§6a "Consequences
worth knowing"), and the WiFi stack's static objects alone force the LVGL pool ≤
~60 kB and starve the draw buffers (`DRAW_BUF_LINES` stuck at 24, §6a). So the
responsibility line moves: the **controller now also owns the profile library, the
device-settings store, and (as the design target — build is §21/D9) WiFi + OTA + the
HTTP data-server.** The CYD keeps LVGL and every view-model — it **renders** the UI,
reads touch, and holds only ephemeral working-copy state; for all *persistent data*
it is a **remote client of the controller** over an expanded `lib/protocol` (§9). As
a direct payoff, the freed CYD DRAM buys **taller render buffers → a faster UI**
(§6a's measured menu).

- **Stays on the CYD:** the whole LVGL UI + view-models, touch, backlight/sleep, and
  **telemetry→SD data logging** — the SD card slot is on the CYD and the controller's
  pins are reserved for the oven, so the log-write path is unchanged (§7).
- **Moves to the controller:** the profile library (store + stock rule + persistence,
  §7/§23), device-settings persistence (§7/§24 — which also gives §4 its
  defense-in-depth per-mode cap on the safety MCU), and the connectivity/OTA design
  (§21/§25/§27).
- **Consequence for the board choice (§6a):** the 3.5" was made default *because it
  survives WiFi bring-up* — but a **radio-less production CYD** never brings WiFi up,
  so that rationale is moot and the 2.8"'s brownout no longer gates the CYD. The 3.5"
  stays default on its size + verified status; the reasoning is what changed. (The
  `_uidev` screenshot server still uses the CYD radio — dev-only, unaffected.)
- **Not built here (design only):** WiFi, OTA, and the data-server on the controller
  remain the future §21/D9 work; the enclosed-CYD **field-reflash path** is a deferred
  open (§21). This revision lands the **storage + protocol** move (the memory win).

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
  its **own** channel (not the control sensor); **bounded total runtime** — a real
  deliverable, not a slogan: at `Start` the controller computes
  Σ(projected segment durations) × margin from `oven_cal.h` and faults
  (`RUNTIME_EXCEEDED`, §22) when a run outlives it; a **per-mode over-temp trip**
  at `hard_max[mode] + margin` (mode derived from recipe content, below) that
  opens the contactor — unlike the setpoint clamp, this acts on *measured* temp,
  so it catches a welded SSR; and a **stuck-heater plausibility fault**: measured
  temp rising past a threshold while commanded duty ≈ 0 for N seconds → open the
  contactor + raise `HEATER_STUCK` (§22). The controller already has duty
  history + wall-TC slope; this is cheap detection, not a redesign.
- **Contactor operating policy (DECIDED):** the contactor coil is
  **energize-to-close** and is closed **only while a run is active** (including
  calibration runs). Idle, faulted, resetting, or sitting in the bootloader ⇒
  coil de-energized ⇒ heater mains removed by construction.
- **Mode-dependent temp cap (two layers — DECIDED):** every run declares a `mode`
  (§9), and each mode is temperature-capped by **two** limits; the lower always wins:
  1. **Absolute per-mode hard-max — firmware constant in the *controller*.** The
     untrusted-proof backstop: it lives in a **hand-written, reviewed header in the
     controller tree** (never the generated calibration file, §6), so even a
     buggy/malicious CYD can't push a run past it. **Reflow = 300 °C (DECIDED)**;
     cure/UV's is a conservative fixed ceiling (value TBD, §10).
  2. **User per-mode max-temp *setting* — device settings, editable on the CYD.**
     Defaults **UV = 100 °C, reflow = 250 °C**. A profile in that mode **cannot be
     authored above its mode's setting** (editor ceiling, §12) and the CYD
     validates before upload. The setting is adjustable only **within** the firmware
     absolute hard-max (layer 1 bounds its range) — never above it; stored settings
     are **clamped to the current hard-max at boot**, so a firmware update that
     lowers the hard-max can never leave a stale higher cap in effect.

  **The cap selector is derived from recipe *content*, not the tag (DECIDED):**
  the `mode` field is authored by the untrusted CYD, so the controller must not
  let it pick the cap. Any segment with `uv=on` or `motor=on` forces the **cure**
  hard-max for the whole run, and a recipe tagged REFLOW that contains `uv=on` /
  `motor=on` — or one mixing UV with setpoints above the cure ceiling — is
  **NAKed** (§9). The tag becomes a cross-checked declaration (UI labeling,
  logging), never the safety selector. The §12 editor doesn't even offer UV/motor
  rows in reflow mode; only calibration recipes may legitimately mix channels,
  and those are cure-capped by this same rule.

  Effective cap = `min(absolute_hard_max[mode], userMax[mode], recipe values)`; a
  recipe can only *tighten*. This **supersedes the old fixed 100 °C cure constant** —
  100 °C is now merely the *default* of the UV setting, not a hard-coded value.
  - **Safety weakening (flagged):** making the UV cap a user setting removes the old
    "UV can never exceed 100 °C" guarantee — a user can now raise it. What remains is
    the **UV absolute hard-max** (layer 1) bounding how high the setting goes, plus the
    independent high-limit. Keep the UV absolute hard-max conservative. As before, the
    L0 hardware cutoff sits at the *reflow* level, so a cure/UV over-temp is held by
    **firmware + high-limit, not the fuse** — but no longer by the setpoint clamp
    alone: the per-mode over-temp trip and the stuck-heater fault (L3 above) act on
    *measured* temperature and open the contactor, so a welded SSR during a cure
    hold is detected and de-powered within seconds instead of riding to the
    reflow-level fuse. That is what makes "acceptable at these temps" true rather
    than asserted.
  - **Enforcement (DECIDED; strengthened 2026-07-17):** the user setting is validated
    **CYD-side** (editor + pre-send), and — now that the **settings store lives on the
    controller** (§2/§7 revision) — the controller **also holds the user per-mode caps**
    and enforces them, not only its absolute hard-max. This **resolves the former open**
    ("optionally also send the user cap to the controller for defense-in-depth"): the cap
    is native to the controller's store, so the defense-in-depth is structural, not an
    extra message. The controller's **absolute** per-mode hard-max (clamp + NAK, §9)
    remains the untrusted-proof limit — layer 1, unchanged.
- **Enclosure over-temp (electronics protection):** on-PCB case sensor (§6),
  two-threshold **warn → auto-abort** to safe state. Protects the SSR/PSU/caps —
  distinct from L0's chamber high-limit, which prevents *fire*; this protects the
  *electronics*. The electronics bay is **ventilated by the donor's magnetron
  fan** (its original job, §6) plus passive vents.
- **Door interlock:** opening the door cuts heater **and UV** via hardware
  (DECIDED — not merely "preferred"), mirrored in firmware. The heater path is the
  donor's switch chain (§6); the UV path is **its own door-operated switch/relay
  in the UV PSU/DC rail** — the donor chain is mains-side and cannot interrupt a
  MOSFET-switched DC load, so without this dedicated path UV-off-on-door-open
  would be firmware-only. On door-open the controller **safes + ends the run**
  (both modes, stateless); **cure resume is reconstructed CYD-side** as a
  remainder profile (§15).
- **UV:** enclose the source; interlock with the door (hardware path above). The
  donor's viewing window blocks 2.4 GHz, **not 405 nm** — fit a **UV-blocking
  sheet** (amber acrylic/film) over it so every cure isn't a chronic blue-light
  exposure for whoever's watching (§6).
- **Fail-safe default:** heater + UV OFF on boot, reset, crash, brown-out, sensor
  fault, or lost link.

## 5. Control & profiles

**Recipe model (DECIDED): generic multi-channel segments.** A profile is an
ordered list of segments; each segment has a duration and a per-channel target +
interpolation (ramp-linear / hold / step). Channels:

| Channel | Reflow use | UV-cure use |
|---------|-----------|-------------|
| `heater_setpoint_C` | the solder curve (peak ~245 °C) | gentle hold ~80 °C |
| `conv_fan` (seg: on/off — duty only if the motor allows it, open §10; phase: Auto/On/Off) | convection during heat (uniformity) | optional |
| `uv` (on/off or duty) | off | on for the cure |
| `motor` (on/off) | unused | turntable — even-exposure under directional UV (§6) |

There is **no chamber `cool_fan` channel** — the teardown found the donor has a convection
fan and an electronics-cooling fan but **no dedicated chamber-cooling fan** (§6, DECIDED),
so cooling is **passive** (heater OFF + coast). Cooling-side control is instead the
**implicit cool-down** below; the electronics-cooling fan runs for the whole of every run
and is controller-managed, not a per-phase setting.

The controller sequences segments, PID-tracks `heater_setpoint_C` via the
zero-cross SSR — against the **mode's control sensor**: the **measured workpiece
thermocouple** in reflow, **raw wall temp** in cure (§6) — sets UV/fans/motor
directly, and streams telemetry + progress (current segment, elapsed, measured
temp). Both modes ride the same executor;
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
  curve/ETA, but *executed* to target), **bounded by the per-segment watchdog
  below** so an unreachable target can't stall the run at full duty.
- **`HOLD`**: keep target for `y` s.
Per-phase channel settings (`conv_fan`/`uv`/`motor`) ride along. This is
why the whole projected timeline + ETA are computable up front (§15).

**Implicit cool-down to touch-safe (DECIDED).** The operator never authors a cool phase.
Instead every profile ends with an **implicit passive cool-down to `kTouchSafeC` (43 °C)** —
cool enough to open the door and handle the workpiece — so the templates carry only the
active phases (reflow preheat/soak/reflow, cure warm/cure). It is realised in **two
independent places**:

- **Compiler-appended (CYD, `implicit_cool.h` + `recipe_compiler.h`):** when a run ends
  hotter than touch-safe, the compiler appends one `RAMP_OVER_TIME` segment down to 43 °C
  whose duration is the passive-coast time on the fan-off cool envelope (`oven_cal.h`), so
  the **projected chamber temp reaches 43 °C at the segment's end**. The preview curve draws
  the same tail (`profile_facts.h`), labelled "Cool". This is an open-loop *estimate*.
- **Controller backup cooldown (`profile_executor.h` + `oven_safety.h` `TOUCH_SAFE_C`):**
  after the last segment the executor holds the heater OFF and stays RUNNING until the
  **measured** control temp confirms 43 °C before reporting DONE (with a generous backstop
  so a stuck-hot sensor can't hang the run — the heater is off throughout, and a genuinely
  stuck-hot chamber is the L3 over-temp trip's job). This closed-loop gate is what
  guarantees the operator is never told a run is finished while the chamber is still hot,
  independent of the compiled tail's accuracy — belt and suspenders.

**Hold-entry gate (reflow — DECIDED):** in reflow, *every* ramp→hold transition
additionally waits for the **measured workpiece temp** (§6) to be within a band of
target. For `x = 0` that *is* the target gate; for `x > 0` the setpoint trajectory
ends on time but the **hold timer doesn't start until the workpiece arrives** — so
hold time (soak, time-above-liquidus) is always counted *at* temperature, against a
measurement, never against a lagging board or an optimistic ramp (§12's amber-flagged
"physically-optimistic" ramps stay saveable; they just take their real time). The
gate is **bounded by the per-segment watchdog** so an unreachable target faults
instead of stalling at full duty. Cure advance stays time-based: at ~80 °C the
stakes are low and cure holds are dose timers, not metallurgy.

**Per-segment watchdog (DECIDED principle, values open §10):** every target-gated
wait (`RAMP_ASAP`, the hold-entry gate) aborts with **`TARGET_UNREACHABLE`** (§22)
when either (a) elapsed time exceeds **k× the projected segment duration** from
`oven_cal.h`, or (b) at saturated duty the measured heat rate stays below a floor
for N seconds. A degraded element, a poor door seal, or low mains voltage then
produces an explicit fault within minutes — not a board baking at 100 % duty just
below every temperature limit until the operator notices. `k`, the rate floor, and
`N` are firmware constants (TBD, §10); the L3 total-runtime bound (§4) is the
independent backstop.

**Fan `Auto` mode (DECIDED — default for the convection fan).** Per phase, `conv_fan`
is **tri-state {`Auto` (default), `On`, `Off`}** (not plain on/off); `uv` and
`motor` stay explicit on/off. (There is no chamber cool fan, so no cool-fan channel to
resolve — cooling is passive, above.) **`Auto` is resolved on the CYD at recipe-compile
time** from `oven_cal.h`: for each segment the CYD picks the fan state **needed to hit the
phase's ramp rate and hold the target** — turn `conv_fan` on when the requested heat ramp
(or hold uniformity) can't be met without it. This keeps the
architecture intact: the **stored profile carries the `Auto` intent** (so it re-resolves
if calibration changes), the **compiled `Recipe` carries the resolved on/off** (§9), and
the **controller stays a generic executor** — no fan policy in the safety MCU.

- **Fan-conditioned calibration (DECIDED):** the calibration fit **always produces** the
  heat-rate envelope modelled **with the convection fan on vs off** (`heatRate(T, conv_fan)`)
  — a standard part of `oven_cal.h`, so `Auto` decides against real fan-on-vs-off rates. The
  cool-rate envelope is fan-off only (passive cooling, no chamber cool fan — §6). The
  characterization runs supply the data by randomizing per-phase fan state (below). See §6.
- **Pre-first-calibration fallback only:** before any calibration exists, `Auto` uses a
  simple heuristic — `conv_fan` on while heating — flagged like the idealized preview (§12).
  Once calibrated, the fan-conditioned envelopes govern.
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
random heater setpoint, random per-phase on/off of `conv_fan` / `uv`,
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

**Control variable (DECIDED — measured, per mode):** the loop tracks the mode's
control sensor directly — **reflow: the permanent workpiece thermocouple** (§6),
the same approach as commercial/hobby reflow controllers (e.g. Controleo3);
**cure: raw wall temp** (a good chamber-air proxy at 80 °C, where precision barely
matters and nobody attaches a TC to a resin print). The `{a,b,τ}` board-temp
estimator is **not in the control path** — it serves planning only
(projection/ETA/feasibility/fan-`Auto`, §6/§12/§15). If direct PI+feedforward on
the long element→air→board lag chain proves hard to tune, a **cascade loop**
(outer workpiece-temp → inner wall-temp setpoint) is the parked enhancement.

Heater control is **PI, not PID** — implemented as a general PID with `Kd = 0` so
D can be enabled later if peak overshoot proves stubborn. Rationale for this plant:

- The plant is slow, **lag- and dead-time-dominated** (sheathed element's own
  thermal mass → forced air → board mass); PI handles first-order-plus-dead-time
  thermal loops well and D's marginal benefit is small. The convection fan raises
  the heat-transfer coefficient (shorter board τ, more responsive) but the element
  stays hot after the SSR opens — a real overshoot source that anti-windup +
  feedforward address better than D.
- **D amplifies noise** — quantized thermocouple-front-end readings (e.g. 0.25 °C
  steps on MAX31855-class parts) next to a
  mains-switching SSR. If ever enabled, D must be **derivative-on-measurement**
  (not on error, to avoid setpoint-step kick) and low-passed hard.
- The main reason to want D (limit **peak overshoot**) is better served by
  **feedforward**: the setpoint trajectory is fully known in advance (scripted
  profile), so a feedforward duty term + PI feedback tracks ramps better than
  leaning on D. Reach for feedforward before D.

**Reference shaping (DECIDED 2026-07-19, from the bench).** The clause above —
"the setpoint trajectory is fully known in advance" — is false for a `RAMP_ASAP`
segment, which is exactly where the overshoot showed up: the executor emits the
segment target immediately, the loop answers a 35 °C error with saturated duty for
the whole ramp, and the element's own mass carries the chamber ~15 °C past setpoint
after the error reaches zero (a 60 °C cure peaked **74.9 °C** on the two-devkit
bench, reproduced by the A10 sim). So the controller now *makes* the trajectory
known: `SetpointShaper` (`lib/control_logic/`) paces the reference toward the target
at the plant's own achievable rate — the same `thermal_math.h` envelopes the CYD's
ETA and §12 preview integrate, so the controller tracks the curve the CYD projects —
with an **approach taper** (rate ≤ remaining/τ, τ anchored to the element's time
constant) that eases duty off *before* arrival, and a lead clamp bounding how far the
reference may run ahead of a plant that cannot keep up. Timed ramps are unaffected
(their setpoint already moves slower than the envelope); descents pass straight
through, since cool-down is open-loop by the rule below. Feedforward is evaluated
along that trajectory (`rampFeedforwardDuty` = holding duty + rate ÷ `rateGain`),
which is what `DutyModel::rateGain` was added for. The shaped reference is never
above the executor's, so this can only reduce commanded heat; `clampSetpoint()` and
the supervisor's last word are unchanged. Sim peak after: **0.25 °C** over
(1.7 °C with the stall-floor guard below). Kd stays 0.

Consequence worth knowing: the taper deliberately slows the approach, and the
executor's per-segment watchdog judges a target-gated wait by the **measured** heat
rate — so the taper carries a floor kept clear of `rateFloorCPerS`, or a run that is
arriving normally gets faulted `TARGET_UNREACHABLE` by its own watchdog. `run_path.h`
`static_assert`s the relationship, since the two constants live in headers that
otherwise know nothing of each other.

Non-negotiables that matter more than P/I/D choice:

- **Anti-windup is mandatory** — the actuator saturates (duty 0–100 %) with dead
  time, so the integrator winds up on ramps; clamp / back-calculate or overshoot is
  guaranteed. Conditional integration alone is **not sufficient** on a shaped ramp:
  it bounds the integrator only by what keeps total duty in range, which on a long
  ramp is most of the range, and that integral then holds the element charged past
  arrival (measured: duty still 0.72 at target, essentially all integral). The
  integrator is therefore **gated to holds** — feedforward owns ramps, feedback owns
  standing offsets — with *unbounded* authority while gated on, so the loop can still
  hold setpoint on feedback alone when the cal is wrong (uncalibrated, it is). The
  trade: while ramping, tracking is feedforward + proportional, so a bad cal shows as
  a standing offset along the ramp — which is precisely what §16's deviation cue
  reports, and what the following hold corrects.
- **Heater is one-sided** (heat only). Cool-down is passive (heater OFF + optional
  fan) → open-loop, not a PID-controlled descent. The loop acts only during
  preheat/soak/reflow.
- **Time-proportioning output:** zero-cross SSR → slow PWM window (~1 s / a few AC
  cycles); PID output = duty.

**UV cure (~80 °C hold)** needs even less — PI on raw wall temp, or bang-bang with
a small hysteresis band, suffices for a steady low-temp soak.

## 6. Hardware & I/O (on the controller — full detail in a future hardware doc)

Loads/sensors hang off the controller ESP32 (GPIO-rich); the CYD only does UI — see §6a for
the HMI board itself.

- **Heater:** a **top-mounted sheathed tubular element** driven via a zero-cross
  SSR (rated ~3.5× element current, heatsinked) + upstream **contactor** + series
  thermal fuse. Gate has a pull-down; may need a transistor/MOSFET to reach the
  SSR's control voltage from 3.3 V. Note the element's own thermal mass = actuator
  lag + a post-shutoff overshoot source (see control loop, §5).
- **Temp sensing (DECIDED — workpiece TC controls reflow; wall TCs control cure +
  backstop everything):**
  - **A few fixed K-type thermocouples on the chamber walls**, each on its own
    thermocouple front-end (shared SPI, one CS per channel) → a spatial map of the
    oven. Heat source is a **top-mounted sheathed tubular element + convection
    fan** → a **mixed convection+radiation** oven, *convection-leaning* (forced air
    is the primary, uniform path; the hot element radiates to line-of-sight top
    surfaces, secondary, strongest at peak). Fan mixing makes wall temp a decent
    air proxy, so wall placement is fine; a TC *in the airflow* would be a more
    direct convective proxy, but the reference-PCB calibration absorbs the
    wall-vs-air-vs-board gap regardless.
  - **Permanent workpiece thermocouple (DECIDED — the reflow control sensor):** a
    machine-installed K-type probe the operator **attaches to the workpiece for every
    reflow run**; the controller **controls and phase-gates reflow on this
    measurement** (§5) — the estimator is out of the control loop. It doubles as the
    **calibration reference TC** (attached to a scrap PCB for characterization runs,
    §20), superseding the earlier calibration-only detachable TC — one channel, dual
    duty.
    - **Probe:** fine-gauge (30–36 AWG) K-type bead for low thermal mass / fast
      response, **glass-braid insulation** (rated ~480 °C; PFA's 260 °C is marginal
      at a 245 °C peak next to hotter walls).
    - **Replaceable:** the probe lead ends in a **miniature K-type connector**
      (high-temp body) mating a panel jack on the shell; a permanent K-type
      extension wire runs from the jack to the probe's own front-end channel.
      Probes are consumables (flexing, tape residue, bead oxidation) — swap the
      probe, never rewire.
    - **Chamber pass-through:** a small hole in the chamber side wall with a
      **ceramic or PTFE bushing** (silicone is marginal at reflow wall temps), sited
      near the jack; enough in-chamber slack to reach anywhere on the reflow rack,
      plus a **stow clip** on the chamber wall for cure/unused runs (stowed, the
      channel simply reads chamber air).
    - **Attachment:** **Kapton (polyimide) tape** pressing the bead flat to the
      board (default — standard reflow-profiling practice); for best fidelity,
      high-temp-solder the bead to a spare pad/via. The reflow rack edge anchors the
      lead (strain relief) so the bead can't be tugged off.
    - **Electrical:** the junction must work **grounded** — the bead touches board
      copper and the board sits on the metal reflow rack, which is electrically
      continuous with the earthed cavity, so a grounded junction is the *expected*
      case, not the exception. The front-end must tolerate that (isolation or
      insulated-junction probe) and must provide **open/short fault detection** —
      both now deciding constraints on the front-end IC choice (§10).
    - **Purchase note (nothing bought yet):** choose the TC junction types
      *together with* the front-end IC — insulated/ungrounded-junction probes (or
      electrically isolated mounting pads) for the wall TCs, and the
      grounded-junction tolerance above for the workpiece probe. Wiring rules for
      the future hardware doc: one controller-GND-to-earth bond point, twisted
      (ideally shielded) TC leads, routed away from the SSR/contactor.
    - **Gating (DECIDED):** reflow `Start` requires the workpiece TC **valid and
      plausible** — non-faulted and reading ≈ the wall TCs at idle (within a band);
      the controller **NAKs** otherwise (§9), and the CYD's Confirm screen shows an
      attach-and-verify row (§19). Mid-run: open/short → `SENSOR_FAULT` abort;
      **not responding while heater duty is high** (plausibility) →
      `TC_IMPLAUSIBLE` abort (§22) — a detached bead reads low/lagging and would
      otherwise make the loop over-drive the board.
  - One thermocouple type covers both reflow (~245 °C) and cure (~80 °C). The
    front-end IC does cold-*junction* compensation internally; the workpiece-TC
    calibration step is about board-vs-wall fidelity, not electrical cold-junction.
  - **Board-temp estimator (DECIDED — planning-only, first-order lag / lumped-RC):**
    a predicted board temperature
    `T_board_est = a · LP_τ(T_wall) + b` — a first-order low-pass (time constant τ)
    of the wall temperature plus affine gain/offset. Fit `{a, b, τ}` (optionally
    per-wall weights) by least-squares against the workpiece TC (on the calibration
    PCB) over the characterization runs. This is the minimal model that reproduces
    the **thermal lag** that starves fast ramps — a static offset/linear fit misses
    it. Bonus: the per-sensor affine term also absorbs each front-end's ±2 °C offset
    (doubles as inter-channel map trim).
    - **Not a control input** (superseded): reflow controls/gates on the *measured*
      workpiece TC, cure on wall temp (§5). The estimator survives for **planning
      and advisory** uses only — the projected curve + ETA (§15), the feasibility
      preview (§12), fan-`Auto` resolution (§5), and the §16 estimator-quality check
      against the measured `workTemp`.
    - If a single gain doesn't hold across 80→245 °C (radiation is ∝T⁴,
      nonlinear), **gain-schedule** `{a,b,τ}` via a small temperature LUT — only if
      single-fit residuals are poor.
  - **Calibration workflow (DECIDED — offline fit, compiled into both):** log
    wall-TCs + workpiece-TC + the random characterization runs to SD (§7); do the
    fit / ML **on a PC**; emit a **generated calibration file**
    (`lib/calibration/oven_cal.h` — fan-conditioned `{a,b,τ}(conv_fan)` +
    fan-conditioned heat/cool-rate envelopes + the feedforward duty model below +
    turntable RPM + UV `beamCoverage`)
    that is **human-reviewed and committed** (the git diff is the gate — no fit
    output reaches a binary unread) and **compiled into *both* firmwares.** One
    source → both binaries identical by construction (same single-source pattern
    as the shared `.proto`); fits the matched-pair invariant (§9). **Safety
    constants are not calibration deliverables:** the per-mode hard-max, max-wall
    ceiling, and trip thresholds live in a hand-written controller header the
    emitter cannot write (§4), and the controller sanity-asserts the cal values
    against compile-time bounds at boot. Supersedes the earlier "controller NVS"
    idea.
    - **Feedforward duty model (DECIDED — a calibration deliverable):** the §5
      feedforward term needs an inverse plant model, which max-rate envelopes
      alone can't provide. The fit therefore also emits **`duty_ss(T)`** (the
      steady-state holding-loss curve) and an effective **duty→heat-rate gain**,
      each per `conv_fan` state — fit from the `heaterDuty` already in every
      telemetry log (§9), a pipeline addition, not a new experiment.
    - Tradeoff: **recalibration = rebuild + re-flash both boards** — now an **OTA
      bundle pushed over WiFi via the CYD** (§21), not a USB reflash, but still no
      *live* on-device recompute. Fine for a single bespoke unit. An optional NVS
      override (firmware defaults, NVS supersedes) could be added later if re-tuning
      gets frequent — not now.
    - Calibration also yields **max heat/cool-rate envelopes** (temperature-
      dependent — heating faster when cold, cooling passive/one-sided), used by the
      feasibility-aware curve preview (§12). Rate-limit + lag math is **shared
      `lib/` logic** reused by the CYD preview and the controller feedforward.
    - **Fan-conditioned envelopes AND estimator (DECIDED — for fan `Auto` +
      planning, §5):** the calibration fit **always produces** the heat-rate envelope
      modelled **with the convection fan on vs off** — `heatRate(T, conv_fan)` — so the
      CYD can decide whether the fan is *needed* to hit a phase's rate. The cool-rate
      envelope `coolRate(T)` is **fan-off only** (cooling is passive, no chamber cool
      fan — §6). The **`{a,b,τ}` estimator is fan-conditioned the same
      way** — `{a,b,τ}(conv_fan)` — because the convection fan shortens the board
      time constant (§5); a single fit over mixed-fan data would average two
      different plants and bias every projection. Planning code switches parameter
      sets with each segment's **resolved** fan state (known at compile time, §5/§9).
      All standard parts of the `oven_cal.h` deliverable, not optional. The
      characterization runs already supply the data by randomizing per-phase fan
      state (§5). The heuristic (§5) is only the pre-first-calibration fallback,
      never the steady state.
  - **Known limitation (now planning-only):** the estimator is fit for one
    calibration board's thermal mass; very different boards lag differently and will
    be mis-projected. Because reflow *control and gating* run on the measured
    workpiece TC, a mismatch no longer distorts the delivered thermal profile — it
    shows up only as projection/ETA error (the run takes longer or shorter than
    predicted) and as a poor §16 estimator-fit verdict. A representative calibration
    board keeps the projections useful; nothing safety- or quality-relevant depends
    on it anymore.
  - **Which variable each limit binds (DECIDED):** recipe setpoints, the user
    per-mode caps (§4), and the controller's absolute per-mode hard-max all bind the
    **mode's control variable** — measured workpiece temp in reflow, wall temp in
    cure. Because holding a board at 245 °C legitimately requires hotter walls/air,
    a separate compiled-in **max-wall ceiling** (sized from calibration's observed
    legitimate wall overshoot + margin) and the **independent analog high-limit**
    (§4) bind **raw wall temperature** — the calibration-independent, physically
    measured backstop.
  - **Open:** exact fixed-channel count; **front-end IC** (undecided — MAX31855 /
    MAX31856 / analog-amp+ADC; must provide **open/short fault detection** and
    tolerate the **grounded-junction workpiece TC** — favors MAX31856-class or
    isolated front-ends). (Fit is offline on PC; §10.)
- **Independent high-limit:** separate over-temp sensor/thermostat on its **own**
  channel, distinct from the control/mapping thermocouples above.
- **Enclosure/ambient sensor (DECIDED):** an **on-PCB I²C digital temp sensor**
  (TMP102/LM75-class, ~±0.5 °C) measuring the case interior — *outside* the chamber
  but *inside* the microwave shell, where the electronics live. Purposes: (1)
  **electronics over-temp protection** (SSR wants <~80 °C, plus PSU / caps / MCU),
  (2) an **ambient ML feature** + starting-temp input to the thermal model, (3)
  operationalizes the "keep the SSR cool" monitoring. **Action (DECIDED):
  two-threshold warn→abort** — UI warning + log at a first threshold, **abort the
  run to safe state** at a higher one (protects SSR/PSU/caps). **Bay ventilation
  (DECIDED):** the donor's **magnetron cooling fan keeps its original job** —
  ventilating the electronics bay — with the SSR heatsink placed in that airflow;
  passive vents + component placement do the rest. (The SSR alone dissipates
  ~13–20 W at element current during ramps, next to a 245 °C cavity — a first-order
  thermal budget of SSR watts + cavity leakage vs airflow belongs in the hardware
  doc.) Threshold values TBD.
  Not a thermocouple (electronics range, not reflow range); MCU-internal sensor is a
  poor fallback.
- **UV LED array (directional, side-mounted — outside the cavity, DECIDED):**
  405 nm via MOSFET (separate from heater), shining in through the old microwave
  antenna/waveguide port — but the array itself lives **outside the cavity behind a
  borosilicate window**, because LEDs (≤85–105 °C parts) cannot share a wall with a
  245 °C reflow chamber, and even an 80 °C cure ambient would derate and age them.
  - **Window mounting:** a borosilicate plate, larger than the port opening, is
    clamped against the *outside* face of the cavity wall by a
    **stainless/aluminum ring-frame** screwed to the wall. The glass floats
    between **ceramic-fiber or fiberglass gaskets** on both faces — never metal
    torqued on glass, and not silicone (marginal at reflow wall temps) — so
    thermal expansion can't crack it. Chamber stays sealed; the beam path stays
    open.
  - **Air gap:** the LED MCPCB + finned heatsink mount on **M3 standoffs
    (10–15 mm)** off the ring-frame, leaving an open gap between window and array;
    the gap is open top/bottom so the electronics-bay airflow (magnetron fan,
    above) washes through it. Conducted heat is broken by the standoffs, radiated
    heat by the gap + airflow.
  - **Output note:** LED output still derates with bay temperature, and the array
    dims as it ages — the photodiode calibration below measures delivered
    intensity, so the dose math tracks reality rather than nameplate.
  - It's a **directional beam from one side**, *not* an all-around source, so a
    stationary part cures unevenly. The **turntable** is what gives even
    all-around exposure (see below) — the two are a functional pair for cure.
- **Convection fan (part of *heating*):** on/off via donor relay is the safe
  assumption; **duty (PWM) only if the motor allows it — the fan's motor type is
  unverified (open, §10):** an AC shaded-pole/synchronous motor can't be PWMed
  (LEDC would be for a DC replacement fan only). Runs during reflow heat phases
  for convective uniformity (top-mounted element would otherwise cause top/bottom
  board ΔT + hot spots).
- **Chamber cooling — passive (DECIDED, teardown-confirmed):** the teardown found the
  donor has a chamber **convection fan** and an **electronics-cooling fan**, but **no
  dedicated chamber-cooling fan**, and we are **not adding one**. Chamber cooldown is
  therefore **passive** (heater OFF + coast + passive vents), and the cool-rate side of
  the plant model is a single fan-off envelope. There is no `cool_fan` channel anywhere in
  the domain, wire protocol, or UI (§5). What replaces active cooldown control is the
  **implicit cool-down to touch-safe** (§5): the compiler appends a passive coast-to-43 °C
  tail and the controller independently backs it with a measured-temp cooldown gate before
  reporting DONE. **The magnetron fan is not a chamber fan** — it keeps its original
  electronics-bay job (above) and runs for the whole of every run (controller-managed, not
  a per-phase setting).
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
  - **Reflow fitout (DECIDED):** the turntable plate is **removed for reflow** and
    replaced with a **metal rack** that holds the workpiece — the turntable + motor
    are cure-only. The rack also anchors the workpiece-TC lead (strain relief, §6
    temp sensing) and, being fixed, removes any lead-wrap concern.
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
      size/position dependent), not a clean geometric constant.
    - **How it gets measured (DECIDED — a 405 nm photodiode coupon):** a small
      photodiode board placed on the turntable during a UV calibration spin —
      wired through the workpiece-TC pass-through to a spare controller ADC
      channel (or a self-contained battery logger) — logs **intensity vs
      turntable angle**; integrating that trace gives a measured `beamCoverage`
      instead of a geometric guess (photodiode part/mount open, §10). **Until a
      measured value exists, `beamCoverage` defaults conservative-low and the §12
      editor labels the exposure→hold conversion "estimated".**
    - **Reflective coating (considering, not decided):** a 405 nm-reflective
      coating on the chamber's inner walls would raise and even out coverage.
      Any coating change invalidates the measured `beamCoverage` — re-run the
      photodiode spin after applying it.
- **Door interlock:** reuse the donor's existing switch chain (see reuse
  inventory) — via the one-line diagram deliverable below, plus the dedicated
  UV-rail door switch (§4).
- **Interlock/mains one-line diagram (REQUIRED deliverable — gates any mains
  wiring, §8):** "preserve, don't bypass" is not a wiring design. Before any mains
  work, produce the one-line diagram: primary + secondary door switches **in
  series with the heater feed**; the **monitor switch's** placement and the fuse
  rating it coordinates with (its dead-short trick is safe only against a properly
  sized fuse); contactor and SSR positions in the chain; verified **switch contact
  ratings** against the element's steady current (a different load profile than
  the magnetron branch they were designed for); and the UV-rail door switch (§4).
  Goes in the future hardware/safety doc; named here so it can't be skipped.
- **Chamber insulation (considering, not decided):** a rockwool layer around the
  cavity — steadier holds, cooler shell. Cost: passive cooldown gets slower, so
  the `coolRate` envelopes shift — recalibrate after fitting it (and reflow
  cool-down rates are a joint-quality parameter, so check the §12 feasibility
  flags still pass).
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
- **Low-voltage SMPS (~5 V logic / 12 V relays).** Candidate to power the
  controller + relay coils — may simplify the power topology (§2) vs a new
  PSU. **Open.**
- **Humidity (steam) sensor** — cure-mode-only, proprietary analog (see above).
- ⚠️ **Remove the inverter/HV/magnetron section** (lethal, holds charge even
  unplugged; qualified-person job). Unused here. (README safety.)

## 6a. Hardware — the HMI board (two CYD variants)

The CYD drives no load, so this section is short by design: its hardware surface is a panel, a
touch controller, a backlight, one UART to the controller (§2) and — on one board — an LDR.
**Two variants are supported**, and the firmware treats "which board" as *config*, not as a
fork:

| | **ESP32-3248S035** (`esp32dev_cyd35`, DEFAULT) | ESP32-2432S028 v3 (`esp32dev_cyd`) |
|---|---|---|
| Panel | 3.5" **ST7796S**, 320×480, run **portrait** | 2.8" **ST7789**, 240×320, run **landscape** |
| Pixel pitch | 6.49 px/mm | 5.60 px/mm |
| Bus | VSPI; **touch shares it** (`bus_shared`) | HSPI; touch on its own bit-banged SPI |
| Backlight | GPIO27 | GPIO21 |
| Link UART (§2) | rx **25** / tx **32** | rx **22** / tx **27** (CN1) |
| Ambient light | **none fitted** | LDR on GPIO34 (§18 auto-brightness) |
| GRAM readback | **no** (SDO unwired) | yes |
| WiFi bring-up (§21) | **survives** | **browns out** |
| Panel contrast | poor — blacks read grey at every backlight level | good |

Both are ESP32-WROOM-32/32E, 4 MB flash, **no PSRAM** (so: partial LVGL draw buffers only,
sized in scanlines — a screen *fraction* would silently double the DRAM cost on the bigger
panel). The link pins move between boards because the 3.5"'s touch shares the display bus,
freeing the 2.8"'s bit-banged touch pins while taking its backlight pin.

**The 3.5" is the default because of the §21 row**, not the extra pixels: without a radio there
is no OTA, and OTA is the controller's only field-reflash path once the oven is enclosed (§25).

> **REVISED 2026-07-17 (§2):** that §21 rationale is now **moot for production** — WiFi/OTA
> moved to the controller, and a **radio-less production CYD never brings WiFi up**, so the
> 2.8"'s brownout no longer gates the CYD. The 3.5" stays default on **size + verified
> status** (and it still brings WiFi up in the `_uidev` screenshot build, which keeps the
> radio). What changed is the *why*, not the choice.

### How "which board" is expressed (the rule that keeps this cheap)

- **`include/cyd_board.h`** is the *only* place that reads a `CYD_BOARD_*` flag. It dispatches
  to that board's `LGFX_*.hpp` and defines its pins, orientation, buffer sizing and
  capabilities. `src_cyd/main.cpp` is a composition root and holds **no pin literal**.
- **Nothing under `lib/` may branch on a board identity** — that is enforced at build level,
  since the native envs have no board at all. `lib/` sees only:
  - **geometry**, via the neutral `PANEL_*` flags (`lib/panel/panel.h`). Layout branches on
    `panel::kPortrait`, never on a board name; a third panel at some other pitch gets the right
    answer with no edit. `theme.h` authors every size in **millimetres** and converts at
    compile time, because the design rules are physical (a 10 mm touch floor is 56 px on one
    board and 65 px on the other). Fonts, which cannot scale that way, are picked by pitch.
  - **capabilities as data** — `subj_has_ambient_light` (a board with no LDR gets a null
    adapter, a disabled "Not fitted" row, and an absolute Screen-brightness control instead of
    an auto-brightness bias) and `device_info.h` (what §24's About reports). One firmware shape,
    no dead branches, and the constants fold away.
- One `[board_*]` section in `platformio.ini` sets both flag families, so they cannot disagree;
  `cyd_board.h` `static_assert`s the pairing anyway, and `#error`s if two boards are selected.

### Consequences worth knowing

- **No on-device screenshots on the 3.5"** (SDO unwired). The dev-tools endpoint returns 501
  rather than serving a perfectly-encoded black PNG — a tool that lies is worse than one that
  refuses. The simulator (`make sim-shot SIM_PANEL=35`) covers layout; the device loop is
  `dev-touch` + the serial trace.
- **A custom draw callback MUST clip to `layer->_clip_area`** — on this hardware that is a
  correctness rule, not an optimisation. With no PSRAM the display renders in **partial
  scanline chunks** (`DRAW_BUF_LINES`), so a `LV_EVENT_DRAW_MAIN_*` callback on the screen
  fires **once per chunk**, ~a dozen times per full redraw. §14's dot matrix originally
  queued every dot on the panel each time: ~7,200 draw tasks per screen instead of ~600, and
  a Home → Settings transition took **~3 s** on real glass. It also compounded with the
  translucent tiles, whose every press forces the canvas beneath them to recomposite. The
  simulator will not warn you — it renders the same pixels, just on a host CPU.
- **The DRAM budget is a hard *link-time* ceiling, and `LV_MEM_SIZE` sits inside it — it is a
  static pool, not runtime heap.** The static `dram0_0_seg` segment holds LVGL's `LV_MEM_SIZE`
  pool (a static array — the linker symbol `work_mem_int`), the WiFi stack's static objects, and
  every app global; only the draw buffers are `malloc`'d from the runtime heap, not from here.
  **Measured 2026-07-17 on the 3.5":** at `LV_MEM_SIZE = 80 kB` a **clean** build overflows
  `dram0_0_seg` by ~15.7 kB. That 64→80 kB popover bump (C5, backlog) was in fact *never* linkable
  from clean — it only ever built because **an `lv_conf.h` edit does not force an LVGL recompile on
  an incremental build** (the "clean if an `lv_conf` edit seems ignored" gotcha), so incremental
  builds silently reused a stale 64 kB pool object and the overflow stayed hidden. Reverted to
  LVGL's upstream default **64 kB**, which fits with headroom (RAM 37.8%) and still runs the
  keyboard popovers. **Runtime heap is comfortable — 105.6 kB free with WiFi joined + the full UI**
  (the `_uidev` build, 60 kB pool). So the binding constraint for a WiFi/OTA firmware is *static*
  DRAM, not heap: the WiFi core's static objects push a 64 kB-pool `_uidev` link ~1 kB over the
  edge, so a radio build needs the pool ≤ ~60 kB (or moved onto the heap via a custom LVGL
  allocator). OTA itself has runtime room. **Always verify a memory change with a clean firmware
  build (`pio run -t clean`), never an incremental one** — an incremental link can pass while a
  clean link overflows.
  - **OUTCOME 2026-07-18 — the §2 "CYD is a UI remote" move landed (Waves R3b/R4), and the
    memory story is subtler than "WiFi frees DRAM".** Two corrections to the earlier
    optimism: (1) **production never linked WiFi** — only the `_uidev` screenshot build does —
    so there were no production WiFi statics to reclaim; and (2) the remote client that
    replaced the on-CYD stores **added** static DRAM (the ManagementClient's message caches +
    the async view-model caches), *more* than the stores freed. Net, `dram0_0_seg` got
    **tighter**, not looser: to fit, the LVGL pool went **64 → 60 kB**, the library's two
    per-mode view-models collapsed to one, and the client's ProfileList/Profile caches were
    unioned. The keyboard-popover 80 kB pool is **not** restorable — the opposite happened.
    **But the draw-buffer win was real, because those buffers are *heap*, not `dram0_0_seg`:**
    a radio-less production CYD (plus LittleFS/NVS gone from the heap) has ample heap, so
    **`DRAW_BUF_LINES` rose 24 → 48** (~28 % faster Home redraw, verified booting on the
    3.5"). The `_uidev` build (WiFi) overrides *down* — pool 58 kB and buffers 24 lines — via
    the now-`#ifndef`-guarded `LV_MEM_SIZE`/`DRAW_BUF_LINES`. **Clean-build rule held
    throughout:** every static change was verified with `pio run -t clean` (an incremental
    link hid the overflow otherwise).
- **The 3.5"'s contrast is a real design input, not a defect to tune out.** Its blacks sit grey
  at every backlight level, which is a property of the glass, and the §14 palette is built on
  colour against near-black — so it has less headroom here than the palette assumes.
  **CHECKED ON GLASS (2026-07-16) — the near-black canvas holds.** The "Azure Instrument"
  palette (§14) was flashed to this panel and reviewed by eye: the azure accent separates
  cleanly from the canvas, the 1 px hairlines and 2 px corner brackets both survive, and the
  dot matrix reads. The hedge — lifting the canvas rather than deepening it, since no code
  change can make this glass darker — was **not** needed and is not in the tree. Re-check if
  the panel is ever swapped or filtered.

## 7. Persistence & configuration

**Profile storage — REVISED 2026-07-17: the controller owns it, on its onboard flash
via LittleFS (DECIDED).** The profile library, its store, and its stock/user rule
move to the **controller** (§2 revised division of labor — the CYD ran out of DRAM
to host it alongside LVGL, §6a). The controller holds the whole library in its
internal flash (LittleFS); the CYD **edits** profiles as a remote client over the
expanded protocol (§9 profile-management messages). The controller still compiles /
executes the single active run's recipe from the accepted `Recipe` (§5) — that path
is unchanged; what moved is *persistence + browsing*. Profiles live in **flash, not
on the SD**, so a run never depends on a card. (The microSD *is* used — for data
logging, on the **CYD**; see below.)

> *Superseded phrasing (kept for history): "CYD onboard flash via LittleFS; the CYD
> owns the profile library." The CYD is now the editing UI, not the owner.*

**Separate per-mode libraries (DECIDED):** cure and reflow profiles live in
**independent stores** — separate LittleFS directories (e.g. `/profiles/cure/`,
`/profiles/reflow/`) each with its own stock seed set — and are **never mixed**. A run
can therefore only ever load a same-mode profile, so the §4 mode cap and §12 template
always match. Browsing/CRUD is in §23.

**Authoring (DECIDED): on-device editing + PC authoring.** Profiles are
created/edited on the CYD touchscreen; **Save writes through to the controller's
store over the link** (§9 `ProfilePut`), no longer to a local LittleFS. PC authoring
stays useful for seeding versioned defaults and bulk edits. The CYD UI still **owns
the profile editor** (§ editing UI below) — editing is UI; only *persistence* moved.
Because a profile is now a **typed wire message** (`Phase`/`Profile` in the shared
`.proto`, §9), the same schema a future PC authoring tool would target is the one the
controller stores.

**Transfer path (for PC-authored profiles):** profiles live in the **controller's**
flash, not on the SD card (so a run never depends on a card), so two mechanisms, not
mutually exclusive:

- **Baseline:** keep default profiles in a repo `data/` dir; `pio run -t uploadfs`
  packs them into the **controller's** LittleFS image (the stock seed set). Versioned,
  simple, needs a USB reflash of the controller.
- **On-demand:** push a one-off profile into the store without reflashing — over the
  **CYD link** (§9 `ProfilePut`, the mechanism the on-device editor already uses) or,
  once the controller has WiFi (§21/D9), a local-WiFi upload endpoint **on the
  controller**. Either way the controller's store validates the untrusted blob (§23) —
  it is the safety MCU, so it hardens the deserializer against a malformed push.

**Editing UI (DECIDED — full create + edit):** the on-device editor builds/edits
profiles as a uniform per-phase **{target temp, ramp `x`, hold `y`}** form (`x = 0`
= as fast as possible) + per-phase channel toggles, compiling to the generic segment
recipe (§5). Layout designed in §12.

Calibration correction factors (from the workpiece-TC calibration workflow, §6) live in a
**generated `lib/calibration/oven_cal.h` compiled into both firmwares** (offline PC
fit → committed file → both binaries identical), *not* NVS. See §6.

**Device settings — REVISED 2026-07-17: persisted on the controller (DECIDED).**
User preferences — units, per-mode **max-temp caps** (UV/reflow, §4), sleep/brightness
constants, WiFi — persist in the **controller's** store (its LittleFS, a single blob
beside the profile library), separate from the profile library. The Settings screen
(§24) reads them at entry and writes them back over the link (§9 `SettingsGet`/`Put`);
the CYD boots on the shipped **defaults** (UV max 100 °C, reflow max 250 °C, §4) and
adopts the stored values when the link comes up (a ~1 s window where brightness/sleep
use defaults — harmless). Stored values are **clamped to the current hard-max at boot**
(§4) so an update can't leave a stale higher cap.

- **This resolves the §4 defense-in-depth open.** With the user per-mode caps now
  living on the controller, the safety MCU can enforce them too (not only its absolute
  hard-max) — the user cap is no longer merely CYD-side policy. The controller's
  **absolute** per-mode hard-max still governs independently and remains the
  untrusted-proof backstop (§4 layer 1).

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
  `workTemp` (the permanent workpiece TC, §6) is logged like every channel;
  calibration runs attach it to the scrap calibration PCB (§20).
- **One file per run**, named by run-id + profile.
- **Retrieval:** the **SD stays permanently inserted** on the **CYD** (the board with
  the slot — the controller's pins are reserved for the oven, §2). Logged files are
  pulled over WiFi, not by removing the card; physically pulling the card is the
  offline fallback. **Open (§10), created by the §2 revision:** WiFi now lives on the
  **controller** (§21), but the logs live on the **CYD's** SD — so the data-server
  either runs on the CYD's own dev-radio (undoes the memory win), or the controller's
  server reaches CYD-SD logs over the link (a file-bridge over `lib/protocol`), or the
  logging itself later moves to a controller-side card. Deferred with the rest of §21/D9;
  logging (telemetry→CYD-SD) is unaffected in the meantime.
- ⚠️ **The real SD risk is heartbeat starvation, not a touch-SPI conflict.** (The
  oft-cited SD↔touch VSPI conflict doesn't apply to this board — touch is on its
  own bit-banged software SPI, per the hardware-bringup skill.) SD cards routinely
  stall 100–500 ms (occasionally >1 s) during internal wear-leveling/GC; the CYD
  must emit a heartbeat every 200 ms against a 750 ms controller timeout (§9), so
  a card stall in the same loop aborts the run. **Requirement (DECIDED): isolate
  heartbeat TX + STOP/touch handling from SD latency** — a dedicated FreeRTOS
  task/core owns the link, fed telemetry through a **RAM ring buffer** that a
  lower-priority task drains to SD; measure the worst-case card-stall budget
  against the 750 ms timeout on the real card (§10).
- **Opens (§10):** wall-clock timestamps without an RTC/WiFi (run-relative time +
  run-id may suffice); ring-buffer sizing vs measured card stalls.

## 8. Build sequencing (MVP: UV cure first)

First mode to reach end-to-end is **UV cure** (lower stakes than reflow-grade
heat), but it rides on the safety/link foundation. **Sequencing invariant
(DECIDED): the heating element never runs — not even at "gentle" cure temps —
until the complete hardware safety chain is in place.** All safety hardware is
installed alongside the rest of the hardware fitout, not deferred to a later
hardening step (a shorted SSR doesn't care that the setpoint was only 80 °C).

1. **Foundation:** `lib/protocol` + UART link + heartbeat/ACK/NAK; controller
   fail-safe skeleton (outputs default OFF, watchdog, command-timeout) proven with
   a *dummy* load (LED "heater") — it shuts OFF within the timeout when the CYD's
   TX is pulled or the CYD reboots. **No mains yet.**
2. **Hardware fitout + complete safety chain (gates all mains heat):** the
   interlock/mains **one-line diagram** (§6) first, then wire it: donor
   door-interlock chain + cavity/element cutoffs integrated (L0), series thermal
   fuse bonded, contactor, independent high-limit, zero-cross SSR sized +
   heatsinked in the ventilated bay, UV-rail door switch + window film (§4).
   Fail-safe proof from step 1 re-run against the real chain — **including step 1's
   second clause, "or the CYD reboots"**, which must be observed with nothing re-arming
   heat afterwards. (The A8 bench stimulus used to auto-start a run on CYD boot, which
   made that clause untestable: the cut fired correctly and the stimulus re-energized
   seconds later. Removed 2026-07-20; the run is started from Confirm, §19, which also
   makes the re-run exercise the production path.) Also the first test of
   **link-on-UART0** with real hardware — the bench pinout is not production's (§2).
3. **UV cure path:** CYD cure engine → upload cure recipe → controller runs gentle
   heat (~80 °C) + UV on + timer (+ optional turntable); telemetry + progress back;
   one wall-thermocouple channel live. One full mode working.
4. **Reflow path:** multi-thermocouple mapping, workpiece-TC install (pass-through,
   panel jack, probe, reflow rack — §6) + the calibration workflow,
   PID tuning on a bench heater, the solder curve, live temp graph on the CYD.
5. **Custom PCB (same ESP32 target, §2):** spin the board, move the bench
   controller firmware onto it unchanged (same module, same adapter), re-verify
   safety on the real hardware. (The bench dev board stays as the reference/dev
   target.)
6. **Connectivity services (§21):** WiFi data-download server + OTA (CYD self-update
   and controller-through-CYD reflash). Last, because it needs the link + calibration
   pipeline to exist first (data to serve, a matched pair to keep in sync) and the
   controller's GPIO0/EN control lines wired (§2).

## 9. UART protocol contract (`lib/protocol`) — the "single API"

Cadence numbers are the accepted defaults (§ table below).

### Link layer (DECIDED)

- UART, **115200 8N1**. CYD **GPIO27=TX / GPIO22=RX** (CN1, second hardware UART,
  *not* UART0) crossed to the controller's **UART0** (its ROM-loader pins, §2);
  3.3 V both sides, no level shifting. Signal cable carries **TX/RX + GND as a
  twisted triple** (§2 interconnect — the signal return stays out of the power
  cable's loop).
- **Encoding: protobuf payloads (nanopb) framed by TinyFrame.**
  - The `.proto` schema is the **single shared contract** — codegen compiled into
    both firmwares (contract can't drift) and reusable by a future PC
    profile-authoring tool.
  - Protobuf is *not* self-delimiting and carries *no* checksum, so each nanopb
    message rides in the payload of a **TinyFrame** frame, which supplies the
    delimiter + integrity: SOF marker + length + type/ID + **CRC-16** (config
    upgradeable to CRC-32; our frames are small so CRC-16 suffices) + parser resync
    on timeout. Chosen over PacketSerial/COBS because TinyFrame is **plain C, no
    Arduino-`Stream` dependency** → drops into `lib/protocol`, links on any
    target, and is host-testable. It is *framing-only*, so it doesn't impose its own
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
  `Calibrate` — they change persistent controller state, so must land
  exactly. (`Abort{}` is fire-and-forget *and* implied by stopping the heartbeat —
  belt and suspenders.)
- **Request/reply path (seq + data-reply, retried until the reply arrives) — ADDED
  2026-07-17:** the profile-library + settings management the §2 revision moved to the
  controller (browse/read/write/delete profiles, get/put settings). Same seq +
  single-outstanding + dedup discipline as the setup path, but the response carries a
  **payload** (a profile list, a profile, a settings blob), not just a verdict — a
  genuinely new shape (request→data vs setup's request→verdict). It is its own
  reliability pair in `lib/protocol` (below) so it never touches the
  authorization-bearing setup/hot paths. Idle-context UI traffic, never in the run loop.

### Message set (protobuf messages)

CYD → controller:

- `Hello{ protoVer, schemaHash }` → handshake at boot (schemaHash gates the link,
  see below). **`Hello` — both directions — and its TinyFrame type id are a
  version-frozen, append-only bootstrap contract (DECIDED):** existing field ids
  and the frame-type id never change, new fields are only appended, so any two
  firmware versions can always decode each other's `Hello`. Everything else may
  churn behind the schema gate; `Hello` is what the gate itself (and the §25 OTA
  verify step) stands on, so it must be decodable across arbitrary skew.
- `Recipe{ id, mode, repeated Segment{ durMs, heatC, convFan, coolFan, uv, motor, interp } }`
  — `convFan`/`coolFan` are **resolved on/off** (the phase-level fan `Auto` is already
  applied CYD-side at compile time, §5); no `Auto` value crosses the wire.
  → upload the whole generic multi-channel recipe in one message (§5). `mode`
  (CURE/REFLOW) is a **cross-checked declaration**: the controller derives the
  effective cap from recipe *content* (`uv=on`/`motor=on` ⇒ cure cap) and NAKs a
  mode/content mismatch (§4) — the executor stays generic; the tag is for
  labeling/logging, never the cap selector.
- `Start{ session, recipeId }` → begin a run for this session.
- `Heartbeat{ session, seq, enable, millis }` every **200 ms** → liveness + run
  authorization. Absence/`enable=false` → controller safes.
- `Abort{}` (immediate safe, sessionless) — sent by the §15 STOP button and any
  CYD-side fatal condition. (No graceful `Stop`: nothing sends one — a run either
  completes, is aborted, or dies with the heartbeat.)
- `Calibrate{ … }` → workpiece-TC calibration hooks (§6, TBD).

*(No `Resume` command — cure resume is a fresh `Start` of a CYD-generated remainder
profile; the controller stays a stateless executor, §15.)*

controller → CYD:

- `Hello{ fwVer, caps, schemaHash }` → firmware id, capabilities, schema fingerprint.
- `Ack{ seq }` / `Nak{ seq, reason }` → setup-command result (Nak on bad CRC /
  out-of-range / illegal transition / **mode/content mismatch** (`uv`/`motor` in a
  reflow-tagged recipe, or UV mixed with setpoints above the cure ceiling, §4) /
  workpiece TC faulted-or-implausible on a reflow `Start`, §6).
- `Telemetry{ session, seq, ctrlMillis, repeated wallTemp, workTemp, boardEst,
  caseTemp, humidity, doorOpen, setpoint, heaterDuty, convFan, coolFan, uvDuty, motor,
  segIdx, elapsedMs, runState, faultCode }` every **250 ms** (4 Hz). `runState`
  (IDLE/RUNNING/DONE/FAULT) drives the CYD's run/summary screens — **no PAUSED**
  (the controller has no pause state; "paused" is a CYD-side UI concept, §15).
  Carries the **full raw vector** (not just a UI subset):
  it doubles as the **SD log record** (§7) — the UI graphs a subset, the CYD writes
  the whole message. `ctrlMillis` is the controller-side sample timestamp (timing
  source of truth). `workTemp` is the permanent workpiece TC (§6) — always
  populated when the channel is non-faulted; it reads chamber air when stowed
  (attachment can't be sensed electrically, hence the §6 plausibility gating).
  Rate is **~1 Hz when idle** (keeps Home's chamber temp + link indicator live, §14)
  and **4 Hz during a run**. The controller also sends an **immediate unsolicited
  telemetry on `doorOpen` change** so the CYD can wake the display fast (§17).
- `Done{ session }` → profile finished cleanly.
- `Fault{ session, code }` → run aborted to safe state (over-temp, sensor fault,
  lost link, …).

### Profile & settings management (ADDED 2026-07-17 — the §2 revision)

Moving the profile library + device settings to the controller (§2/§7) means the
**profile domain model itself is now a wire type**, and the CYD needs a message family
to browse/read/write it as a remote client. All of this rides the **request/reply
path** (above): idle-context, seq-correlated, single-outstanding, retried until the
reply lands, deduped so a resent request never double-applies.

- **New shared types (the domain model on the wire):**
  - `FanMode { AUTO, ON, OFF }` — the tri-state a *stored* phase carries. (Only the
    compiled `Recipe` carries resolved on/off, so `Segment.conv_fan` stays a bare
    bool; the store keeps the `Auto` **intent** so it re-resolves if calibration
    changes, §5.)
  - `Phase { name, target_c, ramp_s, hold_s, exposure_per_surface, uv, motor,
    fan_mode }` — mirrors the CYD's editable `Phase` (the ramp-`x`/hold-`y` domain
    form, §5/§12), bounded `repeated Phase` ≤ `kMaxPhases` (32) and `name` ≤
    `kPhaseNameCap` (16) in `oven.options`.
  - `Profile { mode, name, stock, repeated Phase }` — one stored profile.
  - `ProfileSummary { name, stock, peak_c, total_s }` — the row facts (§23) the
    **controller** computes (it compiles the shared `oven_cal.h` too), so one list
    request renders the whole library without a round-trip per row.
  - `Settings { fahrenheit, uv_max_c, reflow_max_c, brightness/sleep fields, … }` —
    the device-settings blob (§24).
- **CYD → controller (requests):**
  - request→data-reply: `ProfileListReq{ seq, mode }`, `ProfileGetReq{ seq, mode,
    name }`, `SettingsGetReq{ seq }`.
  - request→verdict (Ack/Nak): `ProfilePut{ seq, Profile }`, `ProfileDelete{ seq,
    mode, name }`, `ProfileDup{ seq, mode, src, dst }`, `ProfileRename{ seq, mode,
    old, new }`, `SettingsPut{ seq, Settings }`. The controller's store owns the
    dup/rename **collision resolution** and returns the final name (so the CYD doesn't
    round-trip once per naming attempt, §23).
- **controller → CYD (replies):** `ProfileList{ seq, repeated ProfileSummary }` (≤
  `kMaxListed`), `ProfileData{ seq, Profile }`, `SettingsData{ seq, Settings }`; plus
  the shared `Ack{ seq }` / `Nak{ seq, reason }` for the verdict requests. New
  `NakReason`s: `NAK_STOCK_READONLY`, `NAK_NAME_INVALID`, `NAK_NOT_FOUND`,
  `NAK_STORE_FULL`.
- **Untrusted-input note (the store is on the safety MCU now).** These messages carry
  operator/PC-authored profiles into the controller, so its store hardens the
  deserializer exactly as the CYD's did (§23) — bad magic/version/bounds/name rejected,
  never mis-parsed — and it is added to the controller's fuzz surface (`fuzz/`,
  alongside the frontdoor/decode/validator harnesses).
- These types are **behind the schema-hash gate** (only `Hello` is frozen), so adding
  them is a normal matched-pair reflash. New TinyFrame type ids take `0x19+` in
  `lib/protocol/messages.h`.

### Session & safety semantics

- **Enable = fresh HB for the active session.** No latch. Command-timeout **750 ms**
  (~3–4 missed HBs) → heater/UV OFF + safe state.
- **Session id** prevents a rebooted/stale CYD from authorizing a run it doesn't
  know about (it has no session post-reboot → controller times out → safe).
- Controller **NAKs** out-of-range uploads (upload-time validation); separately,
  the runtime **setpoint clamp** (§4 L3) bounds the *active* setpoint during
  execution as an always-on defense. Two different mechanisms at two different
  times; it never trusts the CYD past a limit either way.
- **Re-sync after a controller reset mid-run (DECIDED):** the controller is
  stateless, so a watchdog/brownout reset loses the session. On boot it
  **unconditionally sends `Hello` + IDLE telemetry**, reads its **reset cause**
  (ESP32 reset-reason register, via the `IWatchdog` port §11), and after a
  watchdog/brownout reset emits a **session-less `Fault{WATCHDOG}`** so the CYD
  learns *why*. **Heartbeats for an unknown session are ignored** (controller
  stays safe/IDLE — they authorize nothing). CYD rule: a RUNNING screen receiving
  session-mismatched or IDLE telemetry raises the §22 overlay ("controller
  restarted — run lost"); its own heartbeat-timeout overlay covers the window
  before the controller comes back.
- Independent of the protocol entirely: the L0 hardware chain (§4) still removes
  power if firmware/SSR fail.

### Schema-consistency check — DECIDED

Plain link CRC catches bit errors but not *version skew*: a frame from a firmware
built against a different `.proto` can transmit with a perfect CRC and be
mis-decoded. One mechanism guards against that:

- **Telemetry arrival is the CYD's liveness signal (DECIDED, A9).** The link is asymmetric: the
  CYD's `Heartbeat` proves the CYD alive *to the controller*, and nothing proves the reverse.
  `Hello`/`matched()` cannot fill that gap — it answers "did a peer once answer, and do we agree
  on the `.proto`", and it **latches**, so it happily reports a healthy link over a cable that
  came out ten minutes ago (it did exactly that on the bench). So the controller emits
  `Telemetry` **unconditionally at 250 ms, run or no run**, and the CYD treats its *arrival* —
  not its contents — as proof the controller exists: `CydLink::linkAlive()`, false until the
  first frame and after `kLinkTimeoutMs` of silence. That drives Home's indicator + run-flow gate
  (§14) and is the producer for §22's `FaultController.linkHealthy`. The two timeouts are
  deliberately different numbers: 1000 ms here (be honest within a second) vs
  `FaultController.linkTimeoutMs` = 2000 ms (be slow to throw a red mid-run modal).
- **Hello carries a per-boot nonce; a peer that hears must answer (DECIDED, A8).**
  Announcing stops once a peer's `Hello` is seen — but *hearing* a peer is not the
  same as *being heard*, so whoever hears first would go quiet without confirming
  and leave the other side announcing into a void forever. This is not a corner
  case: the CYD spends ~3 s in its boot self-test before it starts listening, so it
  loses that race every time, and it also broke the re-sync above outright (a
  rebooted controller could never re-match, meaning **every watchdog reset would
  take the link down permanently** — the watchdog would recover the MCU and kill the
  link doing it). So `Hello` carries a random `boot_nonce`, re-rolled each boot, and
  a received `Hello` is **answered**: immediately if the nonce is new or changed (a
  peer that just booted certainly does not know us), otherwise at most once per
  `kHelloRetryMs`. The rate limit is what makes it terminate — an answer is never
  "new" to a peer that already knows us, so it cannot bounce back — while still
  recovering an answer lost on the wire. *(Found on the A8 bench, not in the host
  tests: both facades `begin()` at the same instant there, which hid the race.)*
- **Handshake schema-hash gate (strict, fail-closed).** A build-time fingerprint
  of the shared `.proto` (hash of its compiled `FileDescriptorSet`) is baked into
  both firmwares as a constant and exchanged in `Hello.schemaHash`. **Mismatch →
  the controller refuses to leave safe state and the CYD shows an explicit
  "controller/UI schema mismatch" error.** Byte-exact: *any* schema change breaks
  the link until both boards are reflashed. This deliberately trades away
  protobuf's rolling-upgrade ability for fail-closed simplicity — acceptable
  because **the CYD and controller are always flashed as a matched pair**
  (invariant). Clearer than MAVLink's silent frame-drop: you get a diagnostic.

(A second, per-frame mechanism — folding the message-type id into TinyFrame's
checksum via `TF_CKSUM_CUSTOM16` — was **considered and deleted**: stock TinyFrame
already checksums the header including TYPE, and a type byte both sides compute
identically catches no version skew; the real MAVLink CRC_EXTRA trick seeds the
CRC with a *message-definition* digest, which the handshake gate makes redundant
on a matched-pair point-to-point link.)

### Firmware transport is out-of-band (note)

OTA (§21) does **not** ride this protocol. **REVISED 2026-07-17:** with WiFi + OTA
re-homed to the controller (§2/§21), the controller **self-updates over its own
radio** (standard ESP32 A/B) — no in-system-programming over the UART, which is what
the earlier "CYD tears the link down and speaks the ROM loader" text described. The
CYD's own field-reflash path once WiFi leaves it is a **deferred open** (§21). What is
unchanged and still load-bearing: any half-applied pair is caught by the **schema-hash
gate** — the frozen `Hello` contract stays decodable across any version skew, so a
stale ↔ new pair holds the system in safe state until both boards match (§21).

> *Superseded (kept for history): the controller had no radio, so the CYD reflashed it
> by driving GPIO0+EN into the ROM serial loader (esptool/SLIP) over the link and
> readback-verifying against the bundle manifest (§25). That whole leg goes away once
> the controller OTAs itself.*

### Code shape

`lib/protocol` = pure C++ (no Arduino/LGFX): frame encode/decode, CRC, recipe
(de)serialize + range-validate, and the run state machine. Host-tested in
`native_logic`. The real UART sits behind a transport port (an in-memory pipe in
tests). Linked into **both** firmwares so the contract can't drift.

The three reliability shapes each get a matched sender/responder pair, all header-y
pure C++: hot (`HeartbeatSender`/`TelemetrySender` + `HeartbeatMonitor`), setup
(`ReliableSender`/`SetupResponder`, request→verdict), and — ADDED 2026-07-17 —
**request/reply** (`RequestClient`/`RequestResponder`, request→data-reply) for the
profile/settings management above. The new pair mirrors the setup pair's seq +
single-outstanding + dedup discipline but delivers a payload reply; keeping it a
separate pair is deliberate, so management traffic can never perturb the
authorization-bearing setup/hot paths the `SessionGate`/`SafetySupervisor` depend on.

### Cadence/params (DECIDED — accepted defaults)

| Param | Value | Note |
|-------|-------|------|
| Baud | 115200 8N1 | safe over short wires; recipe upload is tiny |
| Heartbeat period | 200 ms | CYD→controller liveness+auth |
| Command-timeout | 750 ms | miss ~3–4 HBs → safe |
| Telemetry rate | 250 ms (4 Hz) | smooth live temp graph — **and the CYD's only liveness evidence** (below) |
| Link timeout (CYD) | 1000 ms | miss ~4 telemetry frames → Home reads "No link" (§14). **TBD §10** |
| Setup ACK timeout / retries | 200 ms / ×3 | then surface a UI error |
| CRC | CRC-16/CCITT | over frame body |

## 10. Open questions

Each item is tagged with the earliest §8 step it blocks — resolve it before
starting that step; untagged detail inside an item inherits the tag.

- **Convection fan motor type** *(gates §8 step 2)*: verify whether the donor's
  convection fan is an AC shaded-pole/synchronous motor (relay on/off only) or
  can be replaced by a DC fan for duty control — decides "on/off vs duty" across
  §5's channel table, §6's driver, and the `Recipe` schema (§9). ~~Also verify
  whether a dedicated chamber-cooling fan exists or one must be added.~~
  **RESOLVED (teardown): no chamber cooling fan; cooling is passive, none added** — the
  `cool_fan` channel is dropped everywhere and replaced by the implicit cool-down to
  touch-safe (§5/§6).
- **Donor reuse (Toshiba ML2-STC13SAIT)** *(gates §8 step 2)*: characterize the
  **humidity sensor's** analog interface (cure-mode only); decide **relay-board
  reuse** for on/off loads vs new drivers; decide whether to **reuse the donor
  SMPS** (5 V/12 V) for the controller + relay coils vs a new PSU (§2 power
  topology); whether to log the donor **NTC** as a bonus input. (§6)
- **Thermocouples** *(gates §8 step 3)*: exact fixed-channel count; **front-end
  IC** (MAX31855 vs MAX31856 vs analog-amp+ADC — must provide **open/short fault
  detection** and tolerate the **grounded-junction workpiece TC**, §6); junction
  type of the purchased probes is chosen together with it (§6 purchase note).
  (Estimator model, PID source, NVS storage, fit-runs-on-PC all decided — §6.)
- **Temp-cap limits (§4)** *(gates §8 step 3 — must exist before any mains
  heat)*: reflow firmware hard-max **decided at 300 °C**; still TBD: the
  conservative **UV/cure absolute ceiling** that bounds how high the UV setting
  may be raised, the **max-wall ceiling** (§6 — needs calibration data + margin),
  and the over-temp-trip / stuck-heater margins + times (§4). Defaults now
  **UV 100 °C / reflow 250 °C**. Open: whether to also enforce the user cap
  **controller-side** for defense-in-depth.
- **`.proto` + deps** *(gates §8 step 1)*: finalize the schema fields; add
  `nanopb` + `TinyFrame` to both PlatformIO envs and wire up the protobuf codegen
  step. (§9 — contract is designed; this is the mechanical follow-through.)
- **Controller test lane** *(gates §8 step 1)* in the three-tier native/embedded
  setup.
- **Per-segment stall policy** *(gates §8 step 4)*: the values behind
  `TARGET_UNREACHABLE` (§5/§22) — timeout multiplier `k` (× projected duration),
  the saturated-duty heat-rate floor + its N seconds, and the §4 L3
  total-runtime margin.
- **Profile editor UI** *(§8 step 3)*: structure/layout now designed (§12);
  remaining detail — advanced-mode reorder UX, and building it in the sim.
  (Numeric entry designed: keypad-first per the §24 >20-step rule — phase fields
  open the §26 keypad directly; the stepper serves only nudge-range settings.)
- **Profile transfer** *(§8 step 3)*: `data/` + `uploadfs` baseline, and/or a
  serial/WiFi on-demand push? (§7)
- **Fault overlay (§22)** *(§8 step 3)*: finalize the `faultCode` enum in the
  shared `.proto` + the CYD's code→plain-language table; the buzzer pattern +
  RGB-LED behavior for annunciation (and whether ack silences vs waits for
  condition-clear).
- **Settings (§24)** *(§8 step 3)*: which thresholds are user-exposed vs firmware
  constants (confirm the split); whether raising a temp cap needs more than the
  amber caution; manual brightness-bias range; global "restore defaults" scope.
- **Touch-target sizing** *(§8 step 3)*: numeric entry is resolved — the §26
  keypad backs every wide-range field and the §24 stepper the nudge-range ones
  (>20-step rule), all keys ≥56 px. Remaining
  check: all Settings **toggle rows** (units, auto-brightness, WiFi) render as
  full-width ≥56–67 px rows, and every `‹ Back`/prev-next chevron has a ≥56 px
  hit area despite being drawn small.
- **Profile library (§23)** *(§8 step 3)*: ▲/▼ selection-highlight + auto-scroll
  (vs optional flick-scroll); rows-visible count; per-mode
  **restore-stock-profiles** action in Settings; default sort order;
  duplicate-naming scheme; where the Home **mode chooser** (Cure/Reflow) sits
  before the library. Also: `Save as…` in Setup persisting a tweaked working copy
  (§19).
- **Sleep constants (§17)** *(§8 step 3)*: idle timeout (~1–2 min) and the
  safe-touch temperature below which sleep is allowed while cooling.
- **Auto-brightness tuning (§18)** *(§8 step 3)*: LDR→backlight curve/LUT,
  min-floor/max-ceiling, filter/ramp time-constants — tune on real glass.
- **Cure resume (§15)** *(§8 step 3)*: paused-state timeout before the CYD
  discards the remainder. (Resume gesture decided: press-and-hold, §15/§19.)
- **UV cure specifics** *(§8 step 3; photodiode work rides step 4's calibration)*:
  the **405 nm photodiode coupon** part + mounting for the `beamCoverage`
  measurement spin (§6/§20) + **measure turntable RPM**; UV as on/off or PWM
  duty. (Decided: turntable on/off even-exposure under the directional side UV;
  cure hold authored as UV-exposure-per-surface → computed hold time, §5/§6/§12;
  UV door-interlock is hardware, §4; beamCoverage is *measured*, not guessed —
  §6.)
- **Deviation/drift thresholds** *(§8 step 4)*: live-cue band (§15) and
  end-of-run calibration-drift trigger (§16) — sustained-time N + RMSE/max
  thresholds; firmware constants, tune against real runs.
- **Calibration presets (§20)** *(§8 step 4)*: the exact `n`/`m`/coverage/time
  values behind the re-scoped Quick (cure-range) / Standard (full-range heat +
  passive cool) / Thorough (adds fuller passive-cool characterization) presets.
- **Data logging + calibration pipeline** *(§8 step 4)*: the PC-side
  protobuf→CSV/Parquet decoder tool; the analysis that emits the generated
  `lib/calibration/oven_cal.h` (fan-conditioned `{a,b,τ}(conv_fan)` +
  fan-conditioned heat/cool-rate envelopes + the feedforward duty model
  `duty_ss(T)` / duty→rate gain + turntable RPM + UV `beamCoverage`) compiled
  into both firmwares after human review (§6); wall-clock timestamps without an
  RTC/WiFi (run-relative + run-id may suffice); ring-buffer sizing — **measure
  the real card's worst-case write stall** against the 750 ms heartbeat budget
  (§7).
- **Random-profile generator** *(§8 step 4)*: ranges/`n`/`m` defaults;
  pure-random vs structured sampling (e.g. Latin-hypercube) for better coverage.
  (§5)
- **Fan `Auto` resolution (§5/§6)** *(§8 step 4)*: the exact decision rule
  (rate/target margins for turning each fan on). (Fan-conditioned envelopes +
  estimator are committed calibration deliverables — §6; the
  pre-first-calibration heuristic is the only fallback.) Whether a live/reactive
  controller-side variant is ever worth moving fan policy into the controller
  stays parked.
- **Controller PCB** *(gates §8 step 5)*: N × thermocouple front-ends (IC open,
  above), SSR/UV/contactor drivers, connectors, on-board vs off-board safety
  chain, flashing/debug header. **Fixed requirements from §2:** the CYD-facing
  UART lands on **UART0 (GPIO1/3)** — the ROM serial loader's pins — and the
  board carries the GPIO0/EN pull-ups + EN series-R/RC hardening. Module vs bare
  chip for the ESP32-WROOM-32E is the remaining layout choice.
- **~~Controller field-update = OTA-through-CYD~~ → controller self-OTA over its own
  radio (REVISED 2026-07-17, §2/§21)** *(§8 step 6)*: WiFi moved to the controller, so it
  **self-updates** (standard A/B) — the old CYD-as-ISP / ROM-loader-over-UART / GPIO0-EN
  path is retired. **New open: the CYD's own field-reflash path** once WiFi leaves it —
  flash over its (front-panel) **USB** if reachable, or make the **controller its ISP**
  over the link (re-pin the CYD link to UART0). Decide at enclosure time; not needed for
  bench work (both boards flash over USB).
- **Data-server ↔ CYD-SD bridge (NEW open 2026-07-17, §7/§21)** *(§8 step 6)*: logs live
  on the **CYD's** SD but the WiFi/HTTP server now lives on the **controller** — reconcile
  by bridging log files over the link, running the server on the CYD's dev-radio (undoes
  the memory win), or later moving logging controller-side. Deferred with §21/D9.
- **Connectivity / OTA (§21, §25, §27)** *(§8 step 6)*: WiFi provisioning path —
  **on-device join** (§27 setup flow) vs compile-time **`include/secrets.h`**
  (view goes status-only); OTA image **signing/authentication** on a mains
  appliance (local-net single-user — how much is enough?); CYD **partition
  layout** — does the app fit **twice** (A/B slots) alongside LittleFS in 4 MB?
  (measure once built; the controller image stages on **SD**, off the
  internal-flash budget, §25; fallback = 8/16 MB module or
  single-slot+recovery) + rollback behavior; the HTTP data-download endpoint —
  **auth** (open server on the LAN?), **mDNS `oven.local`**, **QR**, and
  on-device **delete-logs granularity** (§27); confirm the controller-first
  **flash sequencing + rollback** keeps the pair matched under any half-applied
  failure.
- **OTA bundle (§25)** *(§8 step 6)*: the bundle **format** (both images +
  manifest with image hashes for the readback verify) and **source** — fetched
  from a configured URL vs uploaded to the CYD's HTTP endpoint; the **ESP32
  serial-loader client** and the GPIO0/EN drive (§2); the recovery-UX for a
  failed controller flash.

## 11. Controller firmware structure (HAL)

Same shape as the CYD's `lib/display_port` (`IDisplay`/`ITouch`) pattern, on the
other side of the UART. A **hardware abstraction layer** splits the controller
firmware into portable logic vs per-chip hardware access, so the same tested logic
runs on the **bench dev board**, the **PCB's ESP32-WROOM-32E** (§2), and on the
**host** with fakes.

*(That cross-reference was aspirational when written — `IDisplay`/`ITouch` were declared and
documented as "the hardware boundary" but had no production adapter, while `main.cpp` called
LovyanGFX directly. They are implemented now: `LgfxDisplay` / `LgfxTouch` / `InjectedTouch` /
`NullAmbientLight` in `src_cyd/`. Note what earned them their keep, because it is the same
test this section applies: **`InjectedTouch`**, which composes the dev-tools touch injector
onto the panel and thereby deletes an `#if` from the indev hot path — a second implementation
of a port, which is the only thing that makes a port more than ceremony. The flush is
deliberately **not** on `IDisplay`: it needs DMA-pushed byte-swapped RGB565 inside a session
held open across a frame's chunks, so a port carrying it would be LovyanGFX with an `I` on the
front — the exact move this section rejects for `IHeaterSwitch`. There is no policy in it to
push up and no second implementation.)*

### Layering

- **`lib/control_port/`** — pure C++ **interfaces** (ports), one per hardware
  capability. No vendor headers → compiles anywhere (incl. `native_logic`).
- **`lib/control_logic/`** — **portable** logic depending only on the ports: the
  safety supervisor, profile executor, PID + feedforward, time-proportioning,
  calibration application, protocol glue. Host-tested.
- **Adapters** (in each firmware env's `src/`): a single `Esp32*` set
  (Arduino-ESP32: LEDC PWM, SPI for the TC front-ends, `millis()`, ESP watchdog +
  reset-reason) serving both the bench board and the PCB — same module, same
  adapter (§2). Thin — just peripheral calls, no decisions.
- **`Fake*` adapters** (test tree): record/inject values; `FakeClock` advanced by
  hand. Let `native_logic` unit-test safety/control deterministically, no board.

`main()` on each target constructs the real adapters and injects them into the
logic; tests inject fakes into the **same** logic. Injection site is the only
divergence between targets.

### Ports (capabilities)

`IThermocouples` (wall array + workpiece, per-channel `{celsius, fault}`) ·
`IHeaterSwitch` (bare on/off SSR gate — see below) · `IContactor`
(energize-to-close; closed only while a run is active, §4) ·
`IUvOutput` · `IFanOutput` (conv + cool) · `IMotorOutput` · `IDigitalIn` (door
interlock, high-limit) · `ICaseTempSensor` · `IHumiditySensor` · `IClock`
(fakeable time — critical for testing timeouts/PID `dt`) · `IWatchdog` (kick +
**reset-cause readback**, so a post-reset boot can emit `Fault{WATCHDOG}`, §9) ·
`ISerialTransport` (the protocol transport, §9). (`ICalibrationStore` (NVS) was
dropped from the baseline — calibration is compiled-in, §6; it returns only if
the parked NVS-override enhancement is ever activated.)

**Storage ports — ADDED 2026-07-17 (the §2 revision).** The profile library + device
settings now persist on the controller (§7), so its port set gains the two storage
interfaces that used to be CYD-side: `IProfileStorage` (keyed blob CRUD — list / read /
write / remove a profile *by name*) and `ISettingsStorage` (single settings blob). The
real adapter is `LittleFsProfileStorage` / `LittleFsSettingsStorage` over the
controller's own LittleFS partition; the `Fake*` adapters keep the store logic
host-testable with no board. The portable **`ProfileStore` + `SettingsStore` logic
moves into `lib/control_logic`** (from `lib/app_logic`), with the store hardened as an
untrusted-input deserializer (it is the safety MCU, and profiles arrive over the wire,
§9/§23). Two small **request-responders** (`ProfileResponder` / `SettingsResponder`)
sit behind `ControllerLink` as new `IMessageObserver` routes — the same seam
`SetupResponder` occupies for the setup path — answering the §9 profile/settings
management requests off the stores.

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

**Safety override & liveness:** `SafetySupervisor.tick()` runs **last** each loop —
after the PID sets duty and the actuator ticks — so safety has the final word over
whatever was commanded; on any fault/interlock/stale-heartbeat it calls
`heater.forceOff()` (and opens the contactor) — the actuator never overrides safety.
(*Corrected in A8, which wrote the first real loop: this said "first", which is wrong
on the merits — ticking first would let the PID's later `setDuty()` overwrite
`forceOff()`'s zeroed duty and let the actuator re-latch a nonzero on-time for a whole
window, i.e. safety would lose to the PID for a full second.*) If the loop *hangs*,
`tick()` stops running and the pin would hold its last state, so the **hardware
watchdog** (→ reset → pull-down → OFF) is the backstop, not the actuator itself.

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

> **REVISED 2026-07-17 (§2):** the editor still mutates an **in-RAM working copy** (a
> local `Phase[]`), so field-editing and the live preview are unchanged and instant. What
> moved is the endpoints: opening a saved profile **fetches** it from the controller
> (§9 `ProfileGetReq` → `ProfileData`) and **Save** issues a `ProfilePut` over the link
> (validated by the controller's store), rather than reading/writing a local LittleFS.
> Save therefore has a brief **saving / save-failed** state. The `Phase ↔ oven_Phase`
> conversion happens at that wire boundary (a small `phase_codec`), so the editor,
> templates, compiler, and preview keep speaking the domain `Phase`.

### Key insight: edit parameters, not the curve

The design rules forbid gestures/drag and mandate constrained numeric input — so you
**don't drag points on a graph**. Instead you edit named phase **parameters** via
the constrained keypad (§26), and a **read-only curve preview** is *derived* from
them. "Curve editor" = parameter editor + non-interactive preview.

### Structure (DECIDED): fixed templates + advanced add/remove

- **Default — fixed phase templates per mode.** Reflow: *preheat / soak / reflow /
  cool*; cure: *warm / cure / cool*. You edit each phase's parameters + per-phase
  channel toggles, not the structure. Matches how solder profiles are specified,
  fits the screen. Create-from-scratch = a new profile seeded from the default
  template. Phases compile to the generic multi-channel segment recipe (§5).
- **Each phase carries an editable name**, seeded from its template role
  (*Preheat/Soak/Reflow*, *Warm/Cure*; an Advanced-added phase seeds *Phase N*) and
  renameable from the phase editor's Name row via the on-screen keyboard. The name is a
  UI/authoring label only — it is stored in the profile but **never crosses the wire**
  (the compiled recipe carries segments, not names), so renaming needs no protocol change.
- **Advanced (progressive disclosure)** — add / remove / **reorder** generic
  segments via up/down buttons (no drag). For non-standard profiles; more ways to
  make an invalid one, so it's behind an explicit "Advanced" affordance.

### Navigation: hub-and-spoke (two screens)

**Overview** — the curve is the one primary job, kept full-width; phases are a
**one-per-row scrollable list** (the earlier 2×2 tile grid didn't survive the
vertical budget: 2×88 px tiles + a 56 px footer left ~8 px for header *and*
curve). Budget: header ~20 + curve ~52 + **two 56 px phase rows visible** +
footer 56 = 240.

```text
┌──────────────────────────────────────┐
│ Edit: LF-245                  ● IDLE  │  header + machine state (~20 px)
│  °C 250│    __                        │
│        │  _/  \_                      │  read-only derived curve (~52 px)
│      25│_/     \__                    │
├──────────────────────────────────────┤
│ ▸ PREHEAT      →150 °C @1.5/s        │  one phase per row (56 px),
├──────────────────────────────────────┤  ▲/▼ move highlight + auto-scroll
│   SOAK         150–180 °C · 90 s     │  (2 visible of N — §23 pattern);
├─────────┬─────────┬─────────┬────────┤  tap/Open → phase editor. Advanced
│ ‹ Back  │    ▲    │    ▼    │ Save ✓ │  adds add/delete/reorder to the list.
└─────────┴─────────┴─────────┴────────┘
```

**Phase editor** — one phase as a **field list** (highlight + `Open`, like §23/§24). No
cramped inline steppers and no three-toggles-on-a-line: each numeric field opens the
shared **constrained keypad** (§26 — phase temps and times span far more than 20
steps, so per the §24 rule they skip the stepper), and each channel is a
**full-width toggle row**.

```text
┌──────────────────────────────────────┐
│ ‹ Back            Soak (2/3)          │  header: back + which phase
├──────────────────────────────────────┤
│  Name                        Soak   › │  ← selected; Open → on-screen keyboard (rename)
│  Target                    165 °C   › │  Open → keypad (§26)
│  Ramp                       ASAP    › │  (Ramp at min = ASAP / MAX)
│  Hold                        90 s   › │
│  Conv fan               [ AUTO ] (on) │  the only chamber fan; tri-state Auto/On/Off;
│                                       │  (…) = Auto resolved from calibration (§5/§6)
│  UV                          [ OFF ]  │  UV/motor: On/Off — cure mode only
├───────────────┬───────────┬──────────┤
│      ▲        │     ▼     │  Open ›   │  move highlight · edit/flip selected
└───────────────┴───────────┴──────────┘
```

- **▲/▼** move the field highlight (auto-scroll — Name + up to 6 fields, ~4–5 visible);
  **`Open`** acts on it: the **Name** row → the on-screen keyboard (rename this phase); a
  numeric field → the constrained keypad (§26; wide-range per the
  §24 >20-step rule); `uv` → flips on/off; a
  **fan** → cycles **Auto → On → Off**. On `Auto`, the row shows the resolved state in
  parentheses (e.g. `(on)`) so you see what calibration chose (§5). Default = `Auto`.
  All real touch targets are the three ~78 px footer buttons.
- **UV and motor rows exist only in cure mode (DECIDED, §4):** reflow phases have
  no UV/motor options at all — the compiled reflow recipe carries them off, and
  the controller independently NAKs a reflow recipe containing `uv=on`/`motor=on`
  (content-derived cap, §4/§9). Only calibration recipes may mix channels.
- Edits apply to the **working buffer** immediately; the **Overview's `Save ✓`** commits
  the whole profile, and leaving the editor without Save discards — so no per-phase
  Cancel/Done control is needed.
- **Move between phases** from the Overview tiles (tap another tile), keeping this screen
  a single-job field editor.

### Design-rule compliance

- **Numeric:** each field opens the shared **constrained keypad** (§26) — phase
  temps/times all exceed the §24 >20-step rule, so no +/− stepper here (hammering
  ± across a 60–250 °C range is the failure the rule exists for) and never inline
  mini-steppers. Channel settings are **full-width toggle rows**. No free text except
  the profile **name** (on save-as) and **per-phase names** (Name row) — both via the
  on-screen keyboard: a compact name-tuned layout with large keys, per-key popover
  previews, and no auto-repeat; the header **Back** is the cancel (no on-keyboard ✗).
- **Safety-limit clamp:** editor ceilings (the keypad's `max`) = the **mode's max-temp *setting*** (device
  settings — default **UV 100 °C / reflow 250 °C**, itself bounded by the mode's
  firmware absolute hard-max — reflow 300 °C, §4). UI prevents authoring an
  over-limit value; the controller still NAKs over-limit uploads and clamps the
  runtime setpoint against its absolute hard-max as backstop (§9).
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
  read-only — computed `= exposure / beamCoverage` from calibration (§5/§6).
  - **The field's semantics are visible, never silent (DECIDED):** the label
    tracks the meaning — **"UV / surface"** when exposure math applies,
    **"Hold (s)"** when it doesn't. If the turntable is **Off** with UV **On**, an
    amber inline note says **"only the facing surface is exposed"** and the value
    is treated as raw seconds. On an **uncalibrated** oven (no measured
    `beamCoverage`, §6) an exposure-authored profile loads with the field
    relabeled **"Hold (s)" + amber "uncalibrated — raw seconds"**; with only the
    conservative default `beamCoverage`, the derived time is labeled
    **"estimated"**. Validation **re-runs whenever the motor toggle or the
    calibration state changes** — flipping Motor two rows below Hold must never
    silently reinterpret the number above it.

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
  message; the CYD computes the preview locally so it stays instant as values
  change. The rate-limit/lag math is **shared `lib/` logic**.
- **Uncalibrated fallback:** a fresh oven with only default constants shows the
  idealized linear curve with an "uncalibrated — preview is idealized" note.
- Read-only chart (no gauge-without-readout, no 3D/gradients).
- *(Optional later: run the setpoints through the full `{a,b,τ}` closed-loop model
  to also show overshoot/soak-settling — the shared model is built to extend to it.)*

## 13. UI navigation map & global chrome

Hub-and-spoke per the **ui-development** skill. Largely **hardware-independent** —
screens talk to the controller via the protocol + capabilities, not specific ICs —
so the UI can be designed before the teardown. Variable channels (turntable,
humidity) are gated by the controller's reported capabilities.

### Screen map

```text
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
  dot alone fails the CVD rule; ~8% of males can't distinguish it). **Home is the one
  exception (§14):** it has no separate link readout — link health is folded into its
  single status badge (dot+word), so the badge itself reads `NO LINK`/`SCHEMA` when the
  link is unhealthy. Same word-paired, colour-never-alone rule; one fewer widget.
- **Footer:** Back (left) + **STOP** (large red, right) — present + armed on every
  *running* screen; never blocked by UI work (machine work is off the LVGL loop).
- **Run-navigation lock (DECIDED — the rule, stated once):** while a run is
  active (`runState=RUNNING`, including the cure PAUSED overlay), **navigation is
  locked to Run/Monitor** — its footer is the full-width STOP with **no Back**
  (the footer rule's one running-screen exception), and every other screen is
  idle-only by construction. "STOP reachable from anywhere a process can run"
  holds because the only screens visible during a process — Run/Monitor (§15) and
  the calibration runner (§20) — carry a persistent STOP. Matches the §17
  sleep-suppression logic.
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

```text
┌──────────────────────────────────────┐
│ Oven Controller                       │  header: title only (link folded into badge)
├──────────────────────────────────────┤
│   ● IDLE            Chamber  24 °C     │  status badge (state+link) + chamber temp (colour by °C)
├───────────────────┬──────────────────┤
│      ☀            │       ⧉           │  two big mode buttons
│    UV CURE        │     REFLOW        │  (~155×110 px, primary, isolated)
├──────────┬────────┴────┬─────────────┤
│ Profiles │  Calibrate  │  Settings   │  secondary row (≥56 px tall)
└──────────┴─────────────┴─────────────┘
```

### State treatments (where the safety UX lives)

The single status badge folds machine state AND link health into one dot+word (the §13
colour-blind rule: always a word, never colour alone), and the chamber readout's digits
are coloured by the measured temperature. Badge priority, highest first:

- **Link lost / schema mismatch:** badge → `NO LINK` / `SCHEMA` (red) + body banner
  `⚠ Controller not responding`; **mode buttons disabled** — no run flow without a
  healthy link (mirrors §9). This wins over any run/temp state.
- **Fault:** badge → `FAULT` (red).
- **Running:** badge → `RUNNING` (amber). A fail-safe: a run normally leaves Home for
  Run/Monitor, but if Home is ever shown mid-run the badge must not read a reassuring
  green. Distinct from HOT so a cold chamber at run start never mislabels as "HOT".
- **Hot after a run (idle & chamber ≥ touch-safe):** badge → `HOT` (amber). Mode
  buttons stay enabled (set up the next run while it cools).
- **Idle & cool:** badge → `IDLE` (green) + touch-safe chamber temp.
- **Chamber readout colour** (independent of the badge): white while touch-safe
  (< `kTouchSafeC`), amber as it climbs, red past the burn line — the colour reinforces
  the digits, never replaces them.

### Visual language established for all screens

- Header = title + status badge (on Home the badge folds link health in; other screens
  carry the separate header link glyph+word per the global chrome above). **The footer
  rule has exactly two exceptions (§13):** the root hub — no Back (it's root), no STOP
  (idle; unreachable during a run per the navigation lock) — and Run/Monitor,
  whose footer is the full-width STOP with no Back. Here, secondary actions live
  in the bottom row instead.
- **Colour only for state** (green idle · amber heating/hot · red fault); mode tiles
  neutral; danger-red reserved. See the palette below — the base is near-black rather
  than grey, but the ~90 %-neutral discipline is unchanged.
- Big-numbers-first readouts.
- **Palette + line-art (DECIDED — "Azure Instrument", chosen from a rendered variant
  sweep):** a **near-black canvas** (`#05070a`) with one **non-state accent**, azure
  `#0a84ff`, carrying structure and live data; the three reserved state hues
  (`#35d07f` green · `#ffb020` amber · `#ff3b30` red) appear for nothing else. Tokens
  live in `lib/ui_logic/theme.h`; nothing outside it holds a colour.

  The **sci-fi look and the ISA-101 discipline are the same visual grammar** — near-black
  base, ~90 % neutral, one cool accent, mono numerics, thin line-art, red/amber alarm
  semantics — which is why this cost a palette and some line-art rather than a redesign.
  The FUI tropes that *do* conflict (everything glowing, ambient motion, tiny thin text,
  gesture reliance, 3-D clutter, unconfirmed hazardous actions) are all things this panel
  rejects anyway. Concretely adopted:
  - **Accent hue is chosen for maximal distance from the state hues, not for looks.** Azure
    beat cyan/teal candidates because a teal accent sits close enough to `IDLE` green to be
    confusable at a glance through a glove. An accent that can be misread as a state stops
    meaning "look here" and starts meaning "something is wrong".
  - **Structure is drawn with widget borders, never glyphs** — a 1 px accent hairline
    outlining every panel (**all four sides**: a rule on the top edge alone reads as an
    underline on whatever sits above it), corner brackets, a faint dot matrix on the canvas.
    The fonts carry ASCII + `°` and only the few Font Awesome icons `lib/ui_logic/fonts/README.md`
    lists — no box-drawing characters; borders also scale with the mm-authored tokens, where a
    glyph would be frozen at one pixel size.
  - **Buttons are bracketed outlines over the canvas, not filled slabs** — which is what
    gives the dot matrix something to show through, and makes press feedback a *larger*
    delta (outline → filled) than a slab changing shade.
  - **Corner brackets mean "this is a touch target" — nothing else (DECIDED).** They are
    2 px (twice the hairline) and sit on the object's outer bounds, so the hairline's ring
    falls inside them and a corner reads as a *thickening of the outline*, not a second mark
    beside it. The rule is enforced by construction: `theme.cpp` draws them from
    `LV_OBJ_FLAG_CLICKABLE` — the same flag the click routing reads — so the affordance
    cannot drift from the behaviour. Home binds that flag to the controller link, so a
    dropped link strips the mode tiles of brackets *and* edge in the same frame, with no
    code in the theme knowing what a link is.
    - **Not everything that consumes a tap is a touch target.** A profile/settings list row
      is *selected*, and the control you press to act on it is the footer's Open button —
      so rows get no brackets. Spending the signal there puts it on the wrong widget and
      makes a list of them read as noise.
    - Disabled controls lose their edge *and* their brackets and recede into the canvas —
      the opposite of LVGL's default, which brightens them.
  - **Press acknowledgement is instant; only the release animates** (0 ms in, ~120 ms
    ease-out). A press is a fact, not a transition, and the surest way never to miss the
    100 ms budget is not to animate it. `lv_theme_default`'s press *grow* is cancelled
    per-button: a `transform` forces LVGL to snapshot the widget to an intermediate layer,
    which on a big tile is far more expensive than the fill change it decorates.
  - **No gradients** (RGB565 bands, no dithering) and **no full-screen `lv_canvas`** (a
    320×480 RGB565 buffer is ~300 kB and there is no PSRAM) — the dot matrix is a
    draw-event that costs only the dirty region.
  - **Motion only at power-on and for abnormality** (the ISA-101 rule): the sole
    steady-state animation is the §22 alarm banner's *fill* pulsing 10 %→40 %; never its
    border or text, because a blinking word is harder to read.
- **Alert vocabulary (DECIDED)** — `theme.h`'s `apply_alert` / `apply_pill` /
  `apply_fault_panel` / `alarm_pulse`, so §13's banners and §22's overlay inherit one
  treatment. `apply_alert` and `apply_pill` are a hue-tinted **wash at 10 %** with a
  full-strength edge and text: the fill only says *which colour the bar is*, and severity is
  ordered by **saturation, not area** — a fully-saturated amber bar would out-shout the red
  one beside it. `apply_fault_panel` is the deliberate exception: **opaque**, with the theme's
  only above-hairline (2 px) edge, because a modal that let the screen show through would
  suggest the screen behind it is still live. Every one pairs colour with a ⚠/✓/✗ glyph **and**
  a word — never colour alone; that redundancy is what carries the state, since a saturated hue
  on near-black cannot reach the 7:1 critical-readout floor (see `theme.h`'s measured table).
  Reviewable without hardware: `make sim-shot SIM_PANEL=35 ARGS="--screen alerts"` renders the
  specimen in `sim/sim_main.cpp`.
- **Typography (DECIDED):** Red Hat Mono SemiBold, one weight, as the single UI font
  (`LV_FONT_DEFAULT`). A squared/technical monospace face for small-size legibility and
  digit disambiguation on the 320×240 panel — distinct `l 1 I` and a **slashed zero**
  (`0` vs `O`) by default, with a fixed advance that keeps numeric readouts
  column-aligned; the degree sign and the state/link symbol glyphs (✓ ✗ ⚠) are merged
  into it so no fallback font is needed. SIL OFL — embeddable in firmware. Generated +
  documented under `lib/ui_logic/fonts/`.
- Mode buttons open a *flow* (→ Setup), not heat directly — not themselves hazardous.

### Behavior implication (DECIDED)

The controller streams **low-rate idle telemetry (~1 Hz)** even when not running, so
Home always shows live chamber temp **and** keeps the link indicator honest. Cheap
protocol addition (idle heartbeat/telemetry) on top of §9.

## 15. Run / Monitor screen

The most safety-critical screen. Primary job: watch the run progress safely, with
the **projected-vs-actual curve** as the centerpiece.

```text
┌──────────────────────────────────────┐
│ Reflow: LF-245        ⚠ HEATING   ✓   │  profile + state badge + link
├───────────────────────┬──────────────┤
│  212 °C   ▲set 210     │ Soak  2/4    │  big actual temp + setpoint · phase
│                        │ ETA  4:32    │  · ETA
├───────────────────────┴──────────────┤
│ °C 250│          projected ·····      │
│       │        ____·····              │  CHART (the star):
│       │  actual                       │   ···· projected (achievable)
│    25 │_/                             │   ─── actual (measured workTemp)
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

- **Ramp:** `x = 0` → drive max heat, **target-gated** (advance when the measured
  workpiece temp reaches target); `x > 0` → ramp the setpoint over `x` s (rate
  `ΔT/x`, clamped to achievable if too fast → warn, §12).
- **Hold:** `y` s at target (time-based) — in reflow the hold timer starts only once
  the **measured workpiece temp** is within a band of target (hold-entry gate, §5),
  bounded by a per-segment timeout.
- This is the "hybrid" advance policy made precise: **ramp target-gated, hold
  time-based** — and because the gates run on the *measured* workpiece TC (§6), the
  soak/reflow genuinely happens *at* temperature. (Cure advance stays time-based —
  its holds are dose timers at a low-stakes 80 °C, §5.)

### Projected curve + ETA, computed from calibration (DECIDED)

- Every phase duration is computable from the compiled-in `oven_cal.h` (§6): an ASAP
  ramp time = `∫ dT / heatRate(T)` across the ramp (numeric — rate is temp-dependent);
  cool phases use the cool-rate envelope; `x > 0` uses `x` (or the achievable time if
  clamped); holds use `y`. → the **whole projected timeline is known up front** →
  **ETA is a real countdown**, computed entirely on the CYD.
- Because ASAP ramps are **target-gated**, ETA is a calibration-based **estimate that
  slips live** if the oven lags (cold board, door, drift) — re-estimated from progress
  each tick. Honest, not fictional.
- **Projected** (dotted, the achievable §12 timeline, computed via the `{a,b,τ}` lag
  model — board-temp terms) vs **actual** (**measured `workTemp`** from `Telemetry`,
  4 Hz, bold, start→now with a now-marker) — measurement against prediction, same
  physical variable. The cure variant plots **wall temp** (its control variable, §5).
- **Deviation cue:** `|actual − projected|` beyond a band → readout **amber** (run
  falling behind / overshooting) — distinct from the L0/L3 over-temp trips.

### Chrome & controls

- Header: profile name + machine-state badge + link indicator (glyph + word, §13).
- Big current-temp readout + setpoint; phase name + count; ETA.
- **STOP:** large, full-width, **immediate** (emergency — no confirm); sends
  `Abort{}` (§9) and aborts to safe state (heater/UV off, contactor open). Never
  blocked (machine work is off the LVGL loop). Footer is STOP alone — **no Back**:
  navigation is locked here for the whole run (§13 run-navigation lock).
- UV / fan / turntable state indicators.
- Fault → the modal alarm overlay (§22).

### Cure variant

Same skeleton, but: the curve is gentle (→ 80 °C hold on wall temp, capped by the UV
max-temp setting — default 100 °C, §4), the
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

  ```text
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

On a **completed** run only (abort/fault skip it — data incomplete), the CYD computes
**two residuals** from the run log, using the **same residual math as the live cue
(§15)**, shared `lib/` logic:

- **Run quality — measured vs projected:** did the *measured* workpiece temp follow
  the projected curve? RMSE / deviation beyond a band **sustained > N s** (sustained,
  so a transient door-open spike doesn't trip it), **and** per-phase target checks —
  did soak/peak actually reach target and hold time-above-liquidus? This is the
  outcome report; with the §5 hold-entry gate it should normally pass by
  construction.
- **Estimator quality — `boardEst` vs `workTemp`:** every reflow run carries the
  measured workpiece TC, so the planning estimator is checked against **ground
  truth** on every run — no longer a self-referential comparison against a
  projection derived from the same calibration.

Beyond a threshold constant → a prominent but non-alarming advisory with a shortcut
into the **Calibration workflow**:

> ⚠ Actual temperature differed from the prediction. This **may** mean the oven
> needs recalibration — or that this board's thermal mass differs from the
> calibration board.

**Honest by design — and now discriminating:** the two residuals separate the causes
the wording names. A large `boardEst`-vs-`workTemp` residual points at the projection
model (board-mass mismatch or estimator drift); a measured-vs-projected miss with a
*clean* estimator residual points at the oven itself (element aging, door seal). The
advisory picks its wording accordingly instead of guessing.

**Tie-ins:** both metrics are written to the **SD log header** (§7); a flagged run is
a calibration-grade dataset candidate for the offline ML, since it carries `workTemp`
ground truth. Threshold values are firmware constants (TBD, §10).

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
  - **Chamber crossing touch-safe** — if telemetry shows the chamber heating past the
    safe-touch threshold while the screen is asleep, it relights so the `HOT` state is
    visible (the wake half of "suppress sleep while HOT").
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

> **Where this guarantee lives (worth stating plainly).** "A run never starts without
> operator confirmation" is enforced **entirely on the CYD**. The controller has no
> notion of operator intent: it accepts a `Start` from any peer that clears the
> handshake and schema gate (§9), because it trusts its single authenticated peer, and
> no in-band token could prove a human was present anyway. Its job is to make a run
> that *has* started safe (L0–L3: interlocks, over-temp trip, stuck-heater, bounded
> runtime), not to adjudicate whether it was wanted. The consequence is a rule about
> **firmware**, not about screens: *no CYD build may command a run without passing
> through Confirm.* A bench build that did exactly that (`CYD_BENCH_LINK`'s A8 boot
> stimulus) is why this paragraph exists — plugging the CYD in energized the heater.
> Deleted 2026-07-20; see §8 step 2.

### Setup — start empty, Load a profile as a template (DECIDED)

A run begins with an **empty working profile** (no phases). You **Load** a saved
profile (§23) to seed it, then optionally **modify it for this run** — edits apply to an
**ephemeral working copy**, never to the saved profile. This makes every run a potential
one-off (realizing the earlier "custom profiles on demand" decision) without polluting
the library.

**Empty (nothing loaded yet)** — Load is the one primary action, a large central button:

```text
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

```text
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

```text
┌──────────────────────────────────────┐
│ Confirm — Reflow                      │
│  Start reflow?                        │
│  Heater to 245 °C · ~6 min · LF-245   │  specific statement (temp + time)
│  Work TC  24.1 °C ✓ matches chamber   │  attach-and-verify gate (§6)
│  ⚠ Turntable out · reflow rack in.    │  fitout reminder (§6)
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
- **Reflow attach-and-verify gate (DECIDED):** the Confirm screen shows the live
  workpiece-TC reading with its validity/plausibility check ("Work TC 24.1 °C ✓ —
  matches chamber", §6) and reminds "turntable out, reflow rack in";
  **HOLD-to-Start stays disabled** until the TC reads valid + plausible (mirrors
  the §20 step-1 pattern). The controller independently NAKs a reflow `Start` if it
  disagrees (§9) — the UI gate is convenience, the controller gate is authority.
- **Start gesture (DECIDED): press-and-hold ~2 s with a fill ring, both modes**
  (reflow = highest thermal energy; cure = UV eye hazard). Releasing early aborts.
- **On commit:** the CYD uploads the recipe + sends `Start{session, recipeId}`
  (§9), then → Run/Monitor (§15).
- **Cure variant:** statement "UV on · 80 °C · 30 min"; warning "⚠ UV light — protect
  eyes, keep enclosure closed"; button "Start Cure"; same door/enclosure-closed gate.
- Confirmations are kept to **only** the hazardous start, so they retain force.

## 20. Calibration workflow

Produces the data behind `oven_cal.h`. **Honest framing:** because the fit/ML is
**offline on the PC** and calibration is **compiled into both firmwares** (§6), this
on-device flow **collects + logs data — it does not compute or apply the
calibration.** It ends by handing off to the PC. It unifies the random-profile
characterization (§5), the workpiece TC in its reference role (§6), and SD
logging (§7).

A step-wizard:

1. **Attach & verify.** Prompt: attach the workpiece TC (§6) to a scrap PCB, place
   in chamber, close door. **Verify the workpiece TC reads valid** (`workTemp`
   carries no front-end fault flag) and the door is closed before `Next` enables.

   ```text
   Work TC:  24.1 °C  ✓ detected
   Door: closed ✓                        [ Cancel ]  [ Next ▶ ]
   ```

2. **Scope preset** — re-scoped around the plant's real time constants (passive
   cool-down of an insulated cavity from 245 °C takes tens of minutes per cool,
   and the passive (fan-off) cool envelope — the only one, since cooling is passive
   (§6) — is observable *only* via those slow passive cools, so it needs **multiple
   observed cools** to fit well):
   - **Quick (~30–45 min):** cure-range only (≤120 °C) — enough to run cure mode
     credibly; characterizes nothing above it.
   - **Standard (~2 h, recommended):** full-range heating + a passive cool.
   - **Thorough (~3–4 h):** adds fuller passive-cool characterization (multiple cools).
   Maps to the random-generator `n`/`m`/coverage (§5, exact values §10).
3. **Hazardous confirm** — press-and-hold (§19): it drives the oven across the **full
   temperature range** for a long time (needed to characterize the heat/cool-rate
   envelopes end to end), bounded by the safety envelope.
4. **Run** — like Run/Monitor but for the sweep; projected-vs-actual is meaningless
   for random profiles, so it shows **live wall TCs + workpiece(PCB) temp** (watch the
   lag), run `k/N`, ETA, and a persistent **STOP**.
   - **UV spin (optional step, cure calibration):** with the 405 nm photodiode
     coupon on the turntable (§6), a short UV-on spin logs intensity vs turntable
     angle → the measured `beamCoverage`. Skippable; until run, `beamCoverage`
     stays at the conservative default and the §12 editor labels dose math
     "estimated".
5. **Done — the honest handoff:**

   ```text
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

> **REVISED 2026-07-17 — WiFi/OTA move to the controller (design target; unbuilt).**
> The §2 revision makes the CYD a UI remote with **no radio in production** (freeing the
> DRAM the UI needs, §6a), so the WiFi services described below **re-home to the
> controller's** ESP32 radio (previously "dead weight for control", now the connectivity
> host). Read every "the CYD does X over WiFi" in this section as "the **controller**
> does X over its radio" unless noted. What that changes, and what stays open:
>
> - **OTA is much simpler:** the controller **self-updates over its own radio** (standard
>   A/B), so the entire "CYD drives GPIO0+EN → ROM serial loader over the UART" leg (§21
>   below, §25) **goes away**. The matched-pair schema-hash gate (§9) still guards a
>   half-applied pair.
> - **The CYD's own field-reflash path is a DEFERRED OPEN.** With WiFi gone it can't
>   self-OTA. Options (decide at enclosure time, §10): flash it over its **USB** if the
>   port is reachable (front-panel HMI — likely), *or* make the **controller its ISP**
>   over the link (mirror of the old direction; needs the CYD link re-pinned to its
>   UART0/ROM-loader pins). Not needed now — bench work flashes both boards over USB.
> - **Data-server vs SD-on-the-CYD is an OPEN (§7/§10):** logs live on the **CYD's** SD
>   but WiFi is on the **controller** — the server must bridge that (controller server
>   reaching CYD-SD over the link), or logging later moves controller-side. Deferred.
> - **Board power (§6a):** the WiFi-brownout risk that made the 3.5" default now applies
>   to the **controller's** 3.3 V rail (its custom PCB is already spec'd for the transient,
>   §2), not the CYD. A radio-less CYD never brings WiFi up.
> - **Build status:** none of this is built — it stays the future **§21/D9** work. The
>   2026-07-17 change lands only the storage + protocol move (§2/§7/§9). The prose below
>   is the original CYD-hosted design, retained as the substrate the controller-hosted
>   version reuses (HTTP file serving, A/B self-update, idle+cool gating, fail-closed
>   staging all carry over unchanged — only *which board* hosts them flips).

The CYD's ESP32 WiFi (dead weight for control) provides two **local-network
convenience services**, both **idle-only** and never required for a run (§1).

### SD stays resident; data served over WiFi (DECIDED)

- The **microSD stays permanently inserted** as the run/characterization datastore
  (§7) **and the OTA bundle staging area** (§25) — no card-shuffling to a PC.
- The CYD runs a small **HTTP server** exposing the logged length-delimited-protobuf
  files (list + fetch) so the offline analysis/ML pipeline (§7, §10) pulls data over
  the LAN. Read-only, local network, served **only from idle** — a run never
  depends on it, and idle-only also keeps HTTP file I/O away from the §7
  link-task/SD-writer isolation. The physical SD-pull path is kept as an
  offline fallback. The **on-device Connectivity & data view** (§27) shows the URL/QR to
  reach it.

### OTA firmware update — both boards, one bundle, via the CYD (DECIDED)

Both firmwares update **together, over WiFi, through the CYD** — reinforcing the
**matched-pair invariant** (§9): they can't drift because they ship and apply as one
bundle.

- **Why through the CYD:** the controller (radio unused by design, §2) has, once
  enclosed, **no reachable USB** (§10) — so the CYD is its *only* practical field-update
  path. The CYD OTAs itself over WiFi **and** acts as an **in-system programmer** for
  the controller over the existing UART link.
- **Two flashing mechanisms, one loader protocol** (both boards are ESP32, §2):
  - **CYD self-update:** standard ESP32 OTA — A/B (`ota_0`/`ota_1`) partitions +
    rollback; the new image is applied on reboot, a bad/mismatched one rolls back.
  - **Controller update (through the UART):** the CYD drops the controller into
    its **ROM serial loader (esptool/SLIP)** and writes + readback-verifies flash
    over the same wires — **not** via the TinyFrame/protobuf protocol (§9), which
    is torn down for the duration (see §9 "out-of-band" note). The CYD firmware
    embeds the one loader client; the link rides the controller's **UART0**, the
    pins the ROM loader listens on (§2).
- **Extra interconnect (HARDWARE IMPACT, §2):** bootloader entry needs the CYD to drive
  the controller's **GPIO0 + EN** — **two control lines beyond the UART signals**,
  carried on P5's TX/RX with the §2 hardening (open-drain drive, controller-side
  pull-ups, EN series-R/RC). Without them, controller OTA falls back to a manual
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
  safety supervisor — and that's safe by construction, not by advice: **before
  entering the bootloader the controller opens the contactor** (idle ⇒ open
  anyway, §4 contactor policy), and the fail-safe **pull-downs hold every output
  OFF** (§4 L2), so the heater branch is isolated while logic power stays up for
  the flash. (No "unplug the unit" step — cutting logic power mid-flash is
  exactly the §25 power-loss recovery case, not a precaution.)

### Build / topology impact

- These services live in the **production firmware**, gated by a runtime **Settings →
  WiFi** toggle — **distinct** from the compile-time `UI_DEV_TOOLS` dev-server
  (screenshot/touch injection, ui-development skill), which stays dev-only. WiFi
  credentials still come from git-ignored `include/secrets.h`; on-device provisioning
  is an open (§10).
- **Flash budget:** the CYD partition table must fit **LittleFS profiles + A/B OTA
  slots** within 4 MB — verify (§10).
- **Power (HARDWARE IMPACT, §2):** WiFi/OTA is the **only** time the CYD's radio powers up,
  and it is a hard current demand — the ESP32 pulls a **~400–500 mA transient** at RF
  bring-up (plus repeated TX bursts during a transfer). The CYD's **3.3 V logic rail must
  sustain that peak**, or radio init browns the ESP32 into a **reset loop** — meaning a
  board that runs the display fine has **no working field-update path** (§25), since OTA is
  the controller's *only* field reflash route (radio-less, no reachable USB once enclosed).
  This bit us on a dev board whose stock (likely counterfeit) **AMS1117 LDO** current-limited
  below the WiFi peak: it held 3.3 V at the display-only idle load but collapsed the instant
  WiFi initialized — even with a healthy 4.9 V input (after bypassing an input protection
  diode that was dropping ~0.4 V) and a 470 µF output cap. A stronger external 3.3 V source
  was the only thing that fixed it. **Requirement for the custom PCB (§2):** power the 3.3 V
  rail from a **buck regulator** (or an LDO genuinely rated for the transient with thermal
  margin — *not* a bare AMS1117), with **bulk decoupling at the ESP32 module**. A supply
  that merely suffices for the always-on display will fail exactly when OTA is needed.
  - **RESOLVED for the dev HMI (measured, not argued):** the **3.5" ESP32-3248S035** board
    **survives WiFi bring-up** where the 2.8" collapses — repeated `esp32dev_cyd35_uidev`
    boots show **1 boot banner, 0 brownouts**, the radio associating (`READY ip=...`) and the
    controller link staying `matched=1` throughout, with no external supply. That is why it is
    now `default_envs`: on this board the §25 OTA path is actually reachable. **This does not
    relax the custom-PCB requirement above** — one board's LDO happening to cope is not a
    design margin, and the finished product still needs a supply specified for the transient.
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

```text
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
| `OVERTEMP_CHAMBER` | Chamber over-temperature | high-limit / per-mode over-temp trip / setpoint clamp (§4 L3) |
| `OVERTEMP_CASE` | Electronics over-temperature | on-PCB case sensor abort (§6) |
| `SENSOR_FAULT` | Temperature-sensor fault | thermocouple open/short (front-end fault detection) — wall or workpiece TC |
| `TC_IMPLAUSIBLE` | Work sensor not responding | workpiece TC static/lagging while heater duty is high — probably detached (§6) |
| `TARGET_UNREACHABLE` | Target not reachable | segment stalled — timeout or heat-rate floor at saturated duty (§5); degraded element / bad seal |
| `HEATER_STUCK` | Heater energized unexpectedly | temp rising at ~0 % duty — welded SSR; contactor opened (§4) |
| `RUNTIME_EXCEEDED` | Run exceeded time bound | total-runtime bound tripped (§4 L3) |
| `LINK_LOST` | Lost communication | heartbeat timeout (CYD- or controller-detected) |
| `WATCHDOG` | Controller reset (watchdog) | supervisor/loop hang → reset; reported from reset-cause on reboot (§9, §11) |
| `INTERNAL` | Controller fault | catch-all / assertion |

- **Unknown code → still informative:** a code the CYD's table doesn't recognize shows
  `Fault <code> — oven safed to a safe state` rather than a blank — defensive even
  though the matched-pair invariant (§9) should keep the tables in sync.
- `LINK_LOST` wording is special (can't confirm live state): *"Lost communication with
  the controller. If a run was active, the heater is off — the controller safes on
  heartbeat timeout, and its outputs default OFF on any reset."* — reassurance via
  the invariant, not a live readback. (Both clauses matter: the timeout covers a
  hung link with a live controller; the reset-default pull-downs (§4 L2) cover the
  case where the controller isn't executing at all — e.g. held in its bootloader —
  where "it safes itself" would be a claim about code that isn't running.)

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

```text
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

```text
┌──────────────────────────────────────┐
│ ‹ LF-245                     ⧉ Reflow │  back + name + mode badge
│ °C 250│      __                       │  read-only derived curve (§12 preview)
│    25 │_ /  _/  \_ ___                │
│       └──────────────── t             │
│ peak 245 °C · ~6:10 · 4 phases        │  key facts
├─────────────┬─────────────┬────────────┤
│    Edit     │     Dup     │   Delete    │  manage-context actions (Delete/Edit stock-gated)
└─────────────┴─────────────┴────────────┘
```

- **`Load` appears in the Pick context only (Setup → `Load`, §19); the Manage context has no
  `Load` (DECIDED, changed).** Editing a profile and *using* one to run are two separate paths
  from Home — Profiles → library → editor (§12) manages them, **UV Cure / Reflow → Setup (§19)**
  picks one and runs it — so the manage detail (above) never offers a run jump: you can't start a
  run while tidying the library. In the pick context `Load` is the primary action: it copies this
  profile into Setup's working buffer and returns to Setup. *(The shipped library, C4, is
  manage-only; the pick context arrives with Setup, C6.)*
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

- **Reflects the controller's store (REVISED 2026-07-17):** the list is a **remote
  view** of the mode's store on the controller (§2/§7) — the screen issues a
  `ProfileListReq` on entry and after any mutating action and renders the
  `ProfileSummary` rows it gets back (§9). Profiles pushed to the controller (over the
  link, or future WiFi §21) appear on the next refresh. Because it's now a round-trip,
  the list has **loading / error** states (a brief spinner on entry; a retry affordance
  if the request `Failed`) that a local LittleFS read never needed.
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

- **REVISED 2026-07-17 (the §2 move):** the per-mode **`ProfileStore`** + its storage
  port moved to the **controller** (`lib/control_logic`, §11). The CYD keeps a
  **`ProfileClient`** (`lib/app_logic`) that speaks the §9 profile-management
  request/reply and caches the current `ProfileList` + one fetched `Profile` in RAM;
  it's the seam the view-model binds to (fake client on host, real link in firmware) —
  host-tested in `native_logic` exactly as the store was.
- A **`ProfileLibraryViewModel`** scoped to one mode owns `lv_subject_t` list/selection
  state **plus a load-state subject** (loading / ready / error) for the new round-trip;
  views bind + render. The detail screen reuses the **shared curve-preview logic** (§12)
  for its read-only chart — that math (`profile_facts`, `oven_cal`) **stays on the CYD**,
  computed from the fetched `Profile`'s phases, so the preview is instant once the
  profile is in hand. Create-on-demand, delete on leave (no PSRAM, §skill).

## 24. Settings

The rarely-visited hub for preferences + maintenance. Reached from Home → Settings (§14),
**idle-only** (never during or with a staged run — some settings, e.g. temp caps, must not
change under a live profile). A **categorized hub → sub-panels**, using the same
glove-safe **▲/▼-highlight + `Open`** list pattern as the profile library (§23) so no
screen relies on precise small-target taps.

### Settings hub

```text
┌──────────────────────────────────────┐
│ ‹ Settings                        ✓   │  back + link indicator
├──────────────────────────────────────┤
│  Display & units                      │  ← selected (highlighted)
│  Temperature limits                   │
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

Every control obeys the sizing rules: **numbers** → one of the two shared numeric
editors per the **>20-step rule** (below): the value-stepper for nudge-range
fields, the keypad (§26) for everything wider; **booleans** → **full-width toggle
rows** (≥56–67 px, the whole row is the target, not an inline `[ON]`);
**choices** → the ▲/▼-highlight + `Open` list. No inline mini-controls.

- **Display & units** — everything about what the screen does, including when you are not
  touching it: temperature **units** (°C/°F toggle; applies everywhere: editor, run, about);
  brightness; and the **idle timeout** before the screen sleeps (~1–2 min default, §17; → the
  shared value-stepper editor, below).
  Brightness follows the board's hardware (§6a), as data rather than as an `#if`: **with** an
  ambient-light sensor it is **auto-brightness** on/off plus a manual **brightness bias**;
  **without** one (the 3.5" board) the auto row is not shown at all and the bias — a trim on an
  ambient reading that does not exist — becomes a plain absolute **screen brightness** (%).
  The **min-brightness floor always applies** either way (HOT/UV/fault must stay legible, §18) —
  not user-defeatable, which is why the absolute control's own range stops above it rather than
  clamping silently against it.
- **Temperature limits** — the per-mode **user max-temp caps** (§4): UV and reflow,
  each edited on the keypad (§26 — their ranges exceed the >20-step rule). The one
  safety-relevant panel.
- *(There is no **Sleep & wake** category.* The **never-sleep-during-a-run** and
  **stay-awake-while-HOT** rules are **not** user-disableable (§17), so they are **not shown at
  all**: a settings list is a list of things you can change, and a permanently-fixed rule listed
  among them is furniture — it costs a line of a small screen and draws the eye to a dead end.
  They were briefly rendered as disabled "fixed" rows; the same reasoning retired the
  auto-brightness row on a board with no light sensor, §6a. Rows for features that are merely
  *not yet* changeable — the hub's "soon" entries — stay, because those become real. That left
  the category one row deep, i.e. a menu level charging a tap for nothing, so **idle timeout
  moved into Display & units**, which is where it belongs anyway: brightness and idle timeout are
  both "what the screen does when you are not touching it".)
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

### Which numeric editor a field gets (DECIDED — the >20-step rule)

Every numeric field's per-field config `{min, max, step, units}` decides its
editor mechanically: if **`(max − min) / step > 20`**, the field opens the
**constrained keypad (§26) directly** — more than ~20 taps on a +/− button to
cross the range is unacceptable. The **value-stepper** (below) is only for
nudge-range fields (≤20 steps end to end), where ± is genuinely faster than
typing. In practice: temps (60–300 °C), phase ramp/hold seconds, and exposure
all go straight to the keypad; idle timeout (1–10 min) and brightness bias (5 %
steps) keep the stepper.

### Temperature-limits panel (safety-relevant — DECIDED)

Two caps, each edited on its **own** keypad screen (§26 — cap ranges fail the
>20-step rule) — **not** two cramped inline steppers on one panel.
The panel is a 2-row list; `Open` edits the highlighted cap.

```text
┌──────────────────────────────────────┐
│ ‹ Temperature limits              ✓   │
├──────────────────────────────────────┤
│  UV cure max              100 °C      │  ← selected (highlighted)
│  Reflow max               250 °C      │
├───────────────────┬──────────┬───────┤
│        ▲          │    ▼     │ Open ›│  move highlight · edit selected cap
└───────────────────┴──────────┴───────┘
```

### Value-stepper editor (shared, glove-sized — DECIDED, nudge-range fields only)

A numeric setting under the >20-step rule (idle timeout, brightness bias) opens
**this one-value-per-screen editor** with **large −/+ buttons (~96 px ≈ 17 mm, in
the design guide's 15–20 mm gloved-industrial band)**. Rationale straight from the
panel math (5.6 px/mm): +/− are **primary controls → 67–84 px minimum**, and two
side-by-side steppers can't hit that on a 240 px-tall panel — so each value gets
its own screen (progressive disclosure, as the guide intends).

```text
┌──────────────────────────────────────┐
│ ‹ Idle timeout                    ✓   │  header (what you're editing)
├──────────────────────────────────────┤
│ ┌────────┐              ┌────────┐   │
│ │        │    2 min     │        │   │  big − · large value · big +
│ │   −    │ (tap value → │   +    │   │  (−/+ ≈ 96×96 px)
│ │        │    keypad)   │        │   │
│ └────────┘              └────────┘   │
│  Range 1–10 · default 2 min           │  min/max + default (context)
├──────────────────────────────────────┤
│ ‹ Cancel                    Save ✓    │
└──────────────────────────────────────┘
```

- **Large −/+ (~96 px), disabled at min/max** (guide: disable, don't hide); value large +
  centered; **tap the value → constrained numeric keypad** (large keys, §26) is still
  there for direct entry; **press-and-hold −/+ accelerates** (the guide's stepper
  pattern).
- Whichever editor a field uses, its ceiling is the field's bound: for a **temp
  cap**, the mode's **firmware absolute hard-max** (§4 layer 1) — the value moves
  only *within* it, **never loosens past it**; the controller enforces the
  hard-max regardless (untrusted-CYD-proof, §4/§9).
- **Commit is a plain button, not press-and-hold** (both editors) — editing a cap
  starts no heat/UV (the §19 Confirm is the real energizing gate). But **raising a
  cap above default** shows the **amber caution** — a visible nudge without
  confirmation friction.
- **The caution string is per-mode, part of the per-field config (DECIDED):** both
  numeric editors are shared widgets configured per field (§26 relationship), and
  the caution renders wherever the field is edited — for the caps, the keypad's
  side rail (§26). The honest wording differs by mode. **UV cap:** "Above
  default — protection is firmware + high-limit only at these temperatures" (the
  L0 fuse sits at reflow level, §4 — claiming "hardware fuse still governs" here
  would be false reassurance). **Reflow cap:** may reference the thermal fuse —
  there it truly governs.
- On **Save**, any profile loaded in Setup is **re-validated** against the new cap (§12
  hard-validation) — lowering below a loaded profile's peak flags it.

### Persistence & behavior

- All settings persist on the CYD (LittleFS/NVS, §7), separate from the profile stores;
  firmware ships the **defaults** (units °C, UV cap 100 °C, reflow cap 250 °C, idle ~1–2
  min, auto-brightness on). "Restore defaults" (global) lives here too.
- Changes apply on the panel's **Save** (or immediately for toggles); idle-only entry means
  none of this races a run.

### Code architecture (per the ui-development skill)

- A typed **`SettingsStore`** with **validation baked in** — e.g. the shared numeric
  editors clamp a cap to the firmware hard-max — host-tested in `native_logic`. **REVISED
  2026-07-17 (§2/§7):** the store + its storage port moved to the **controller**
  (`lib/control_logic`); the CYD keeps a **`SettingsClient`** that reads via
  `SettingsGetReq` on entry (falling back to `settings_defaults.h` until the reply lands)
  and writes via `SettingsPut` (§9), caching the blob in RAM. Both numeric editors are
  **reusable widgets** driven by the same per-field config (min/max/step/units — which
  also decides stepper-vs-keypad, the >20-step rule), used by settings and the profile
  editor (§12) alike.
- Per-panel **view models** expose `lv_subject_t` settings; views bind + render. The temp
  caps publish to the subjects the profile editor (§12) reads for its editor ceilings, so
  a changed cap tightens the editor with no extra wiring. Hub + panels create-on-demand,
  delete on leave (no PSRAM, §skill).

## 25. OTA firmware-update flow

> **REVISED 2026-07-17 (see §21 banner):** WiFi/OTA move to the **controller**, which
> self-updates over its own radio — so the **controller-first, flash-over-UART leg
> below is superseded** (the controller no longer needs the CYD as its ISP). The
> **fail-closed matched-pair invariant survives**: the §9 schema-hash gate still holds
> the pair in safe state on any half-applied outcome. The **CYD's** update path is a
> deferred open (§21). Unbuilt — future §21/D9. The wizard/UX below is retained as the
> reusable substrate; only *which board flashes what* changes.

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

```text
Preflight ─► Review/Confirm ─► [1] Flash controller (UART) ─► [2] Verify controller
          ─► [3] Flash CYD (self, A/B) ─► reboot ─► [4] Post-reboot result
```

1. **Flash controller (over UART).** The CYD drives **GPIO0+EN** (§2) to drop
   the controller into its **ROM serial loader** and writes flash via the esptool/SLIP
   client (§21) — *not* the TinyFrame protocol. The riskiest leg, done **while the CYD is
   still on its good image** so a failure here is fully recoverable.
2. **Verify controller — out-of-band first, handshake second.** Still in the ROM
   loader: **read back the written flash and compare its hash to the bundle
   manifest's** — this verifies the image with zero dependence on any protocol
   version. Then release reset, re-establish the TinyFrame link, exchange `Hello`
   (decodable across any version pair — it's the frozen bootstrap contract, §9),
   and **check `schemaHash` == the bundle's**. Readback-hash or hash mismatch /
   no response → **abort before touching the CYD** (keep a working CYD; offer
   retry).
3. **Flash CYD (self).** Write the new image to the **inactive A/B slot** (`ota_1`), mark
   it to boot, then **reboot**. The old slot is untouched.
4. **Post-reboot result.** The new CYD boots and **commits its slot on
   self-test** — image integrity + app boot-to-UI — **not on link health**: a
   transient UART glitch at that moment must not roll back a good image. It then
   re-handshakes and confirms the pair's `schemaHash` for the success card. If
   the new image is bad (won't boot) → ESP32 **rolls back** to the old slot →
   old-CYD + new-controller = **schema mismatch** → the §9 gate **holds safe
   state** and shows the mismatch, prompting a re-run. Every partial outcome
   converges to *safe*.

### Screens

**Review / Confirm** — plain confirm (reflashing starts no heat/UV, so **not**
press-and-hold, §19/§22; the risk is interruption, not energy):

```text
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

```text
┌──────────────────────────────────────┐
│  Updating firmware                    │
│                                       │
│  [1/2] Controller                     │  which stage
│  ▓▓▓▓▓▓▓▓▓▓▓░░░░░░░░░  57%            │  progress bar + percent
│                                       │
│  ⚠ Keep powered — do NOT switch off.  │  persistent, whole flow
│  Outputs OFF · contactor open.        │  reassurance (§4 L2 + contactor policy)
└──────────────────────────────────────┘
```

**Result** — success or a recovery state (below). Success is really the *next* normal boot
confirming the pair; the flow ends on a `✓ Updated — CYD v1.4 · controller v0.9 · schema OK`
card with **[ Done ]**.

### Failure & recovery (every partial state is recoverable)

| Failure point | State | Recovery |
|---|---|---|
| Bad/mismatched bundle | nothing flashed | reject up front; fix the bundle |
| Controller flash fails | CYD good, controller partial | **Retry** — the ESP32's **ROM serial loader is immutable and always reachable** via GPIO0+EN (§2, given the §10 UART0 routing), so it's **not bricked** |
| Post-flash verify mismatch | controller new-but-wrong, CYD untouched | abort before CYD; retry controller |
| CYD self-update / rollback | old CYD + new controller | §9 schema gate **holds safe** + shows mismatch → re-run OTA |
| Power loss mid-flash | controller partial and/or CYD on old A/B slot | on repower: ROM bootloader + A/B good-slot make it recoverable; CYD detects no-valid-controller / mismatch → offers re-flash |

The reassurance to surface throughout: **the controller ROM bootloader is immutable and
the CYD keeps a known-good A/B slot**, so there is no single step that bricks the unit.

### Safety & design-rule compliance

- **Idle+cool gate**, no heat/UV in the flow; during the controller-bootloader window the
  supervisor isn't running, so **fail-safe pull-downs keep heater/UV OFF** (§4 L2) and the
  **contactor is open** (idle ⇒ de-energized, §4/§21) — the flow shows
  "Outputs OFF · contactor open."
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
  **`ControllerProgrammer`** (GPIO0/EN drive + the ESP32 ROM-loader client,
  host-fakeable), the **`ISerialTransport`** (§11) for the verify
  handshake, and the ESP32 **self-OTA** API. Staging + fail-closed logic is **host-tested**
  in `native_logic` with fakes (simulate flash-fail, verify-mismatch, rollback).
- An **`OtaViewModel`** exposes `lv_subject_t` progress/stage/result; the flashing work
  runs off the LVGL loop (gateway pattern, §skill) so the UI stays responsive. Screens
  create-on-demand.

## 26. On-screen numeric keypad

The **primary editor for every wide-range numeric field** (§24's >20-step rule):
phase target/ramp/hold, exposure, and the temp caps all open **directly here** —
hammering +/− across those ranges is exactly what the rule forbids. For the few
nudge-range fields that keep the value-stepper (§24), the keypad is also reachable
by **tapping the value** there. It's a **shared modal** configured per field with
`{min, max, step, units}` — the same config whose step count picked this editor.

### Layout (DECIDED — familiar 3×4 grid + side rail)

The digits keep the universal keypad arrangement (1-2-3 / 4-5-6 / 7-8-9 /
⌫-0-OK) — decades of muscle memory beat a novel grid. A full-width 3×4 doesn't
fit 240 px (4 rows × 56 px floor = 224, leaving nothing for header + value), so
the grid takes a **left 224 px zone at full screen height** and the field
name/value/range + Cancel move to a **right-hand rail**:

```text
┌─────────┬─────────┬─────────┬────────┐
│    1    │    2    │    3    │ Target │  right rail (~96 px): field name,
├─────────┼─────────┼─────────┤  temp  │
│    4    │    5    │    6    │────────│
├─────────┼─────────┼─────────┤ 245 °C │  live value + units,
│    7    │    8    │    9    │ 60–250 │  range (always shown)
├─────────┼─────────┼─────────┤────────│
│    ⌫    │    0    │  OK ✓   │   ✕    │  ✕ Cancel (≥56 px)
└─────────┴─────────┴─────────┴────────┘
```

- Digit/control keys ≈ **74×60 px** (224/3 × 240/4) — every key clears the 56 px
  floor on both axes, gloved-friendly. `Clear` is dropped: **long-press `⌫`
  empties the value** (⌫ alone recovers any state in ≤4 taps anyway).
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
- **`⌫`** deletes the last digit; **long-press `⌫`** empties; both give instant (<100 ms)
  value feedback. Every key shows a pressed state immediately.
- **`✕` / Cancel** discards and returns to the caller (the field list, panel, or
  stepper) with the **old** value;
  **`OK`** commits the new value back to the field. The keypad never talks to the
  controller — it just edits a number.
- Opens **pre-loaded with the current value** and the field's units/range; sensible.
- **Per-field caution strings render in the rail** (under the range): e.g. raising
  a temp cap above default shows its per-mode amber caution here (§24) — the
  keypad is where caps are edited, so the honesty lives here too.

### Relationship to the value-stepper editor (§24)

One per-field config `{min, max, step, units}` drives both widgets, and its step
count picks the editor (§24 >20-step rule): **≤20 steps → stepper** (± is faster
than typing for a nudge), **>20 steps → keypad, directly** (never route a
250 °C-range field through a ± button). Both clamp to the same firmware bound
(e.g. a temp cap's hard-max, §4), so the rule changes ergonomics, never limits.

### Code architecture (per the ui-development skill)

- A pure **`NumericEntry`** logic class in `lib/app_logic` (append-digit, backspace, clear,
  in-range test, over-max block) — **no `lv_`**, host-tested in `native_logic` (the
  range/clamp/edge rules are exactly the kind of logic to unit-test off-device).
- A reusable **keypad view** binds to it via `lv_subject_t` (typed value, valid flag);
  opened as a modal over whatever invoked it — a §12/§24 field list directly
  (wide-range fields) or the value-stepper (nudge fields, via tap-the-value) —
  returning the committed value through a
  subject. One widget, configured per field — no per-screen keypads.

## 27. Connectivity & data view

Where you go to **get logged data onto a PC** and manage WiFi. Reached from Settings →
**Data & firmware** (the data half; the firmware half is OTA, §25) and Settings →
**Network** (§24). **Idle-only** — all WiFi services are (§21).

> **REVISED 2026-07-17 (see §21 banner):** WiFi now lives on the **controller**, so the
> "WiFi status", QR/URL, and WiFi-setup elements below report/drive the **controller's**
> radio (the CYD sends WiFi-config over the link and renders status it receives back —
> it's still a UI remote here too). The **logs are on the CYD's SD** while the server is
> on the controller — the §7/§21 data-server↔SD-bridge open governs how this screen's
> URL actually reaches the files. Unbuilt (future §21/D9); layout/UX below is retained.

**Key framing:** the actual file browsing/downloading happens in a **PC (or phone)
browser** against the CYD's HTTP server (§21) — so this screen's job is to get you *to*
that server (**URL + QR**) and summarize what's on the SD, **not** to list every file with
download buttons (a poor use of 320×240 when the PC does it better). The header keeps the
global **controller-link** glyph (§13); WiFi status is a separate, clearly-labelled body
line so the two links aren't confused.

### Connected state (the primary view)

```text
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
