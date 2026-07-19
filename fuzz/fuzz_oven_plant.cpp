// Internal-property harness: the thermal-plant twin (A10, lib/plant/oven_plant.h) must keep every
// node temperature FINITE and within a physical band under any sequence of commanded outputs and
// time steps — including the adversarial ones a real caller can't produce but a fuzzer will:
// NaN/Inf/denormal duty, NaN/Inf/negative/huge dt. The plant feeds the safety-critical control
// loop through synthetic thermocouples, so a non-finite reading escaping here would poison the
// executor, PID and supervisor downstream (fuzz_sim_run exercises that closed loop).
//
// Invariant (no oracle): after every step(), all five accessors are finite and in [-100, 5000] °C.
//
// Input format (little-endian; documented so seed_gen could emit an on-contract seed):
//   [0]      config : bit0 → insulated plant (halves the loss UA)
//   then N × 9-byte step records, one step per record:
//     [0..3]  duty : float32 (raw bytes → NaN/Inf/denormal/out-of-range)
//     [4..7]  dt s : float32 (raw → NaN/Inf/negative/huge; the plant must clamp/ignore)
//     [8]     flags: bit0 convFan, bit1 uv, bit2 motor
#include <cmath>
#include <cstring>

#include "fuzz_util.h"

#include "oven_plant.h"

namespace {

constexpr size_t kHeader = 1;
constexpr size_t kRec = 9;

float readF32(const uint8_t *p) {
  float f;
  std::memcpy(&f, p, sizeof(f));
  return f;
}

bool finiteBounded(float v) {
  return std::isfinite(v) && v >= -100.1f && v <= 5000.1f;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size < kHeader) {
    return 0;
  }
  PlantParams pp;
  if ((data[0] & 1) != 0) {
    pp.insulationFactor = 0.5f; // the §6 "considered" insulation — a valid alternate plant
  }
  OvenPlant p(pp);

  for (size_t i = kHeader; i + kRec <= size; i += kRec) {
    const float duty = readF32(data + i);
    const float dt = readF32(data + i + 4);
    const uint8_t fl = data[i + 8];
    p.step(dt, duty, (fl & 0x01) != 0, (fl & 0x02) != 0, (fl & 0x04) != 0);

    FUZZ_ASSERT(finiteBounded(p.chamberTempC()));
    FUZZ_ASSERT(finiteBounded(p.wallTempC()));
    FUZZ_ASSERT(finiteBounded(p.workpieceTempC()));
    FUZZ_ASSERT(finiteBounded(p.bayTempC()));
    FUZZ_ASSERT(finiteBounded(p.elementTempC()));
  }
  return 0;
}
