# Small-Touchscreen Design Guide (glove-operated machine controller)

Distilled reference for designing oven-controller screens on this panel: 2.8″ 320×240
ST7789, resistive XPT2046 touch, operated with nitrile gloves, controlling genuinely
hazardous outputs (heat, UV). The hard pass/fail rules live in SKILL.md; this file holds
the rationale, the numbers' provenance, and the patterns to reach for.

## Panel math

Pixel pitch ≈ 71.1 mm diagonal / 400 px diagonal ≈ **0.18 mm/px → 5.6 px/mm**.

| Physical size | Pixels | Use |
|---|---|---|
| 10 mm | ~56 px | absolute floor for any target |
| 12–15 mm | ~67–84 px | primary controls (Start, Stop, steppers) |
| 15–20 mm | ~84–112 px | gloved-industrial recommendation; hazardous controls |
| 2 mm | ~11 px | minimum spacing between targets |

A comfortable button occupies a meaningful fraction of the screen — only a handful of
controls fit per screen. That is a feature: it forces progressive disclosure.

Sizing provenance (convergent industry guidance): NN/g research-backed minimum
1 cm × 1 cm (Parhi/Karlson/Bederson 2006: ~9.2–9.6 mm sufficient); Apple HIG 44×44 pt;
Material 48×48 dp (~9 mm); Microsoft 9 mm recommended / 7 mm minimum / 2 mm spacing;
WCAG 2.5.5 (AAA) 44×44 CSS px; ISO 9241-9 ~22–23 mm (secondhand figure, older
touch tech — treat as a conservative upper anchor, not a target); industrial display
integrators recommend 15–20 mm for gloved use. Average fingertip is 16–20 mm wide,
thumb ~25 mm (MIT Touch Lab via NN/g) — gloves only add to that.

## The resistive-touch design contract

Resistive panels register pressure between two conductive layers. Consequences:

- **Single-touch only.** Two simultaneous touches average into a useless point. No
  pinch/zoom, no multi-finger gestures, ever.
- **Works with gloves/stylus** (why it's the right tech here), but gloves reduce
  precision and dull tactile feedback → larger targets, stronger visual confirmation.
- **No hover state.** Nothing can be previewed before commitment; affordances must be
  visible at rest (raised/filled/bordered buttons; non-interactive text clearly
  non-tappable).
- **Prefer taps over drags.** Drag friction is high and error-prone with gloves. If a
  slider is unavoidable, give it a large handle and pair it with +/− buttons.
- **Drift and wear accumulate.** Ship an on-screen recalibration routine eventually;
  avoid tiny or edge-crowded targets that drift breaks first.

## Layout and hierarchy at 320×240

- **One primary job per screen** ("Reflow — Running", "Set Cure Time"). The single most
  important datum (current temperature, time remaining) is the largest, most central
  element.
- **Prominence proportional to consequence** (ISA-101): a critical temperature or alarm
  never shares visual weight with a minor label.
- **Group by proximity** (Gestalt): related controls cluster; hazardous actions sit
  visibly apart from benign ones — placing consequential next to benign options is a
  classic top-10 design mistake (NN/g).
- **Consistent chrome:** header = screen title + machine state; footer = Back +
  persistent Stop. No breadcrumbs or tab bars — they burn scarce pixels.
- **Progressive disclosure:** defaults visible, advanced/rare settings behind a
  secondary screen.

## Navigation

Shallow hub-and-spoke: Home (machine status + mode choices UV Cure / Reflow) → short
per-mode flow (profile → parameters → confirm → run/monitor). Borrow ISA-101 display
levels at small scale: L1 at-a-glance status, L2 per-mode control, L3 rare
settings/diagnostics. Minimize taps for common tasks; persistent obvious Back; Stop
reachable from anywhere a process can run.

## Feedback and responsiveness

Nielsen's three response-time limits (from *Usability Engineering*, 1993):

| Limit | Meaning | Rule here |
|---|---|---|
| 0.1 s | feels instantaneous | every tap shows a pressed-state change ≤100 ms — the single biggest perceived-quality lever on a resistive panel |
| 1 s | flow stays uninterrupted | longer → show a working indicator |
| 10 s | attention limit | heating/curing → countdown or percent-done |

Layer modalities: visual pressed-state always; a short confirmation beep and/or haptic
tick greatly improves confidence through gloves ("reduces the need for visual checks" —
valuable when the operator watches the machine, not the screen). Distinct tones for
"cycle complete" vs "setpoint reached" vs fault alarms; keep alarm sounds distinguishable
and intermittent (alarm fatigue mirrors color desensitization).

Architecturally: machine work (heating control, curing timers) never blocks the LVGL
loop — the UI must stay responsive to Stop at all times.

## Typography and legibility

- Primary readouts (temperature, countdown) are the largest on-screen elements, sized
  for glance reading under workshop lighting and safety glasses.
- Contrast (WCAG 1.4.3): ≥4.5:1 normal text, ≥3:1 large text; aim ≥7:1 (AAA) for
  critical readouts. White-on-black is ~21:1.
- Clean sans-serif, open letterforms; no weights below 400 — thin strokes lose contrast
  on a low-res anti-aliased panel.
- Meaning never rides on color alone; pair with text/icons.

## Color discipline (ISA-101 / IEC 63303 high-performance HMI)

Published as IEC 63303 (2024), derived from ANSI/ISA-101.01-2015. The core: ~90% of the
screen stays neutral grayscale; **color means "look here now"**. Aligned with
IEC 60204-1 machine conventions:

- **Red** = danger / alarm / emergency (hot surface, UV on, fault). Reserved — red
  appearing anywhere else dilutes it.
- **Amber/yellow** = warning / abnormal (heating up, approaching setpoint, door open).
- **Green** = safe/normal, used sparingly — "when everything is green, operators become
  desensitized."
- **Colorblind safety:** red–green CVD affects up to ~8% of males of Northern European
  descent (~4.5% of males globally), and red/green is ~99% of all CVD — the worst
  possible pairing. Prefer blue/orange or blue/red contrasts where a pair is needed, and
  always add a redundant cue: explicit word ("UV ON", "HOT 210 °C"), icon, position, or
  blink.
- Pick one color convention, document it in the style guide, apply it everywhere.
  Blue/white indicators wash out under bright light.

## Safety-critical interaction design

Layered protection for heat and UV:

- **Specific confirmations.** "Start reflow — heater to 245 °C?" restates the
  consequence; the meaningful verb goes on the button ("Start Heating"); the hazardous
  button is the red one; default focus/placement favors the safe choice. Never a bare
  "Are you sure?".
- **Confirmations stay rare.** Overused confirmations become reflexive and lose all
  force (NN/g). If users stop reading them, remove routine ones.
- **Friction for the highest-energy actions:** press-and-hold to start, or two-step
  arm-then-start, to defeat accidental gloved taps.
- **Always-available stop** (ISO 13850 spirit): single action, available at all times,
  overrides normal controls, requires no deliberation. On-screen: a large persistent red
  STOP. ISO 13850 treats E-stop as a *complementary* measure — the touchscreen Stop
  never substitutes for the hardware thermal fuse or a physical E-stop (this repo's
  standing safety constraint; see README).
- **Unambiguous machine state**, always on screen: idle / heating / hot / curing /
  fault. Mode errors (believing UV is off when it is on) are the failure class to design
  against hardest.

## Error prevention and numeric input

Prevention beats recovery (Nielsen #5; Shneiderman "prevent errors", "permit easy
reversal"):

- **Constrain, don't validate after the fact.** Steppers/keypads with enforced min/max —
  an out-of-range temperature cannot be entered at all. Disable (don't hide) +/− at
  limits; disable Start until inputs are valid.
- **Steppers (+/−)** for values near a common default: fewest interactions, no invalid
  states. Large, horizontally arranged; press-and-hold to accelerate.
- **Numeric keypad** for wide-range entry from scratch (target temperature). Large keys
  suit gloves.
- **Sliders** only for coarse low-precision settings, always paired with a numeric
  readout and fine +/−. A linked "coarse slider + fine stepper" is a strong reflow
  setpoint pattern.
- Always show current value with units; sensible defaults; round display values (no
  floating-point artifacts); plain-language errors stating problem + fix, never codes;
  offer return-to-safe-default.

## Displaying live data

- **Big numbers first:** large numeric readout with units is the most legible,
  most precise glanceable form on this panel.
- **Reflow trend:** small setpoint-vs-actual line chart, auto-ranged Y axis (ISA-101:
  trends rarely need full range). Minimal — no 3D, no gradients.
- **Cure countdown:** progress bar + remaining time answers "how long?" instantly.
- **Gauges sparingly:** space-hungry and low-density; if used, always show the number
  too, color zones only for warning/danger. Usually a bold readout wins at 320×240.

## Consistency and the style guide

Consistency is rule #1 for both Nielsen (#4) and Shneiderman. Before building screens,
fix a one-page style guide (an ISA-101 recommendation) and reuse it everywhere:

- grayscale base palette + the semantic red/amber/green with mandatory text/icon pairing
- two type sizes (large readout, body) meeting the contrast floors
- standard header (title + machine state) and footer (Back + persistent Stop)
- one button style with a defined pressed state; identical terminology and positions
  across screens

Match real-world mental models: thermometer/heat for temperature, timer for duration.
Show current settings and state rather than making the operator remember them
(recognition over recall).

## Evaluation checklists

- **Nielsen's 10 heuristics:** visibility of status; real-world match; user control;
  consistency; error prevention; recognition over recall; flexibility; minimalist
  design; error recovery; help. Run each new screen through them.
- **Shneiderman's 8 golden rules:** consistency; shortcuts for frequent users;
  informative feedback; dialogs yield closure; prevent errors; easy reversal; internal
  locus of control; low memory load.
- **ISO 9241** foundation: usability = effectiveness + efficiency + satisfaction in a
  specified context (-11); human-centred design is iterative, user-involved,
  whole-experience (-210); dialogue principles incl. self-descriptiveness,
  controllability, error tolerance (-110).
- **Industrial HMI habits worth keeping:** design around operator tasks ("What am I
  making? Is it running right? What next?"), not machine topology; alarm discipline
  (few, meaningful, prioritized; suppress nuisance alarms during heat-up transitions);
  **test with the actual nitrile gloves** and review at ~30/60/90% completion.

## Decision thresholds

- Glove testing shows >~5–10% mis-taps → increase target size/spacing before anything
  else.
- An operator ever misjudges machine state → make state indication larger/redundant and
  add friction to state changes.
- Users reflexively confirm dialogs without reading → too many confirmations; delete
  routine ones so dangerous ones regain force.
- Any tap without visible reaction ≤100 ms → treat as a defect.
- RAM/flash pressure → cut decorative animation and trend history **before** cutting
  target size, contrast, or confirmation feedback.

## Caveats

- Exact millimeter criteria inside paywalled standards (ISO 9241-410) are unverified;
  the ISO 9241-9 ~22–23 mm figure is secondhand and partly reflects older touch tech.
  Apple's 44 pt and WCAG's 44 px are not fixed physical sizes — always convert to mm
  for this panel.
- ISA-101 percentages ("90% grayscale", alarm-rate targets) are process-industry
  conventions, not laws; the principles transfer, the numbers are guides.
- Where a claim drives a **safety** decision, verify against the primary standard
  (ISO 13850, IEC 60204-1, IEC 63303, WCAG, ISO 9241) rather than this distillation.
- A touchscreen Stop is not an emergency stop. Hardware protection (thermal fuse,
  physical E-stop) is the layer that counts; software cutoffs sit on top (README).
