// Programmable fake for IAmbientLight — injected into AutoBrightness under native_logic_cyd so
// the ambient curve/filter/hysteresis are tested deterministically, no hardware. Set `value`
// (raw ADC counts) before ticking the code under test. Header-only, shared via
// `#include "helpers/fake_ambient_light.h"`.
#pragma once

#include "IAmbientLight.h"

struct FakeAmbientLight : IAmbientLight {
  uint16_t value = 0;
  int reads = 0;
  uint16_t read() override {
    reads++;
    return value;
  }
};
