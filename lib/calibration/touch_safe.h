// touch_safe.h — the ONE definition of the touch-safe chamber temperature for the whole codebase.
//
// "Touch-safe" is the chamber temperature a run must cool below before it reports DONE (design.md
// §5/§6): cool enough that the operator can open the door and handle the workpiece. Several
// independent subsystems, on BOTH MCUs, need to agree on this number:
//   - the CYD recipe compiler / preview appends a passive cool-down aiming for it
//   (implicit_cool.h),
//   - the controller enforces its own independent backup cooldown to it on MEASURED temperature
//     (control_logic/oven_safety.h),
//   - the CYD Home badge/readout treats a chamber at or above it as "hot / not touch-safe"
//     (ui_logic/home_viewmodel.h).
// They used to carry their own copies with "keep the two values in step" comments — a latent
// skew bug. This header is the single source; each site derives its symbol from kTouchSafeC below.
//
// Why a shared compile-time constant does NOT weaken the §4/§9 "never trust the CYD" rule: that
// rule is about runtime data flow — the controller must not accept a limit pushed over the wire by
// a possibly-malicious CYD. A reviewed constant baked into each firmware at build time is not wire
// data; the controller still clamps against its OWN compiled-in copy. Sharing the source only
// guarantees the two firmwares were built from the same reviewed number. Change it here, review it
// once, and every consumer moves together.
//
// Lives in calibration/ because that is the codebase's board-neutral, both-firmware thermal-domain
// home (alongside thermal_math.h), NOT because it is a calibration OUTPUT — it is a reviewed safety
// constant and must never migrate into the generated oven_cal.h (oven_safety.h §6 spells out why).
//
// Pure C++: no LVGL, no Arduino, no <cmath> — matching the rest of calibration/.
#pragma once

namespace oven_domain {

// The touch-safe chamber temperature, in deg C. See the header comment for who consumes it.
inline constexpr float kTouchSafeC = 43.0f;

} // namespace oven_domain
