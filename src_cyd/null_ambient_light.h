// NullAmbientLight — the IAmbientLight for a board with no light sensor fitted.
//
// A capability difference expressed as an object, not an #if. The Settings screen that offers
// auto-brightness lives in lib/ui_logic and must never see a board flag, so "this board has no
// LDR" has to reach it as data (subj_has_ambient_light). main.cpp picks this adapter, holds
// AutoBrightness disabled, and publishes the capability — one firmware shape, no dead branches,
// and cyd_board::kHasAmbientLight folds the choice away at compile time anyway.
//
// The alternative — pointing Esp32AmbientLight at kAmbientPin = -1 — is what shipped before this
// existed, and analogRead() truncated the -1 to pin 255 and logged "Pin 255 is not ADC pin!" once
// a second forever, while AutoBrightness read the resulting 0 as "bright room" and pinned the
// backlight at maximum.
#pragma once

#include "IAmbientLight.h"

class NullAmbientLight : public IAmbientLight {
public:
  // Never consulted: AutoBrightness is held disabled wherever this is installed. 0 rather than a
  // mid-scale guess so a reading that somehow *is* used looks obviously synthetic in the trace.
  uint16_t read() override { return 0; }
};
