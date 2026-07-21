// Seed-corpus generator (dev tool, env fuzz_seedgen; plain GCC, no fuzzer/sanitizer).
//
// Emits on-contract starting inputs for each harness by reusing the REAL encoder and
// framer, so the corpus is valid wire traffic rather than hand-typed bytes. Run once
// from the project root; commit fuzz/corpus/**. Re-run and re-commit if the seed set
// or the input formats change.
//
//   .pio/build/fuzz_seedgen/program            # writes every fuzz/corpus/<harness>/ subdir
//   .pio/build/fuzz_seedgen/program <basedir>  # override the corpus base dir
//
// Formats (must match the harnesses):
//   frontdoor : full TinyFrame wire bytes (SOF..CRC), as the controller receives them.
//   decode    : byte 0 = TinyFrame type id, rest = raw nanopb payload.
//   validator : byte 0 = recipe-payload length L, next L bytes = Recipe, rest = Start.
//   compiler  : the recipe-compiler harness's own struct format (header + phase records, see
//               fuzz_compiler.cpp). Not a wire encoding — this harness fuzzes a CYD-side producer,
//               so there is no protocol message to reuse the encoder for; the seed is hand-packed.
//   setpoint_shaper : the reference-shaper harness's format (plant-model/config header + tick
//               records, see fuzz_setpoint_shaper.cpp). Hand-packed for the same reason.
//   heater_control : the PI-loop harness's struct format (gain header + tick records, see
//               fuzz_heater_control.cpp). Also hand-packed — controller-side control math, no wire
//               message to reuse the encoder for.
//   safety_supervisor : the L3-gate harness's struct format (recipe + driver records, see
//               fuzz_safety_supervisor.cpp). Hand-packed; controller-side safety logic.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <vector>

#include "ISerialTransport.h"
#include "codec.h"
#include "frame_link.h"
#include "messages.h"
#include "oven.pb.h"
#include "schema.h"

namespace {

namespace fs = std::filesystem;

// Captures everything a FrameLink writes, so send() yields the wire bytes.
struct CaptureTransport : ISerialTransport {
  std::vector<uint8_t> buf;
  size_t write(const uint8_t *data, size_t len) override {
    buf.insert(buf.end(), data, data + len);
    return len;
  }
  size_t read(uint8_t *, size_t) override { return 0; }
};

struct NullHandler : protocol::IFrameHandler {
  void onFrame(uint8_t, const uint8_t *, size_t) override {}
};

void writeFile(const fs::path &path, const std::vector<uint8_t> &bytes) {
  std::FILE *f = std::fopen(path.string().c_str(), "wb");
  if (f == nullptr) {
    std::fprintf(stderr, "seed_gen: cannot write %s\n", path.string().c_str());
    return;
  }
  if (!bytes.empty()) {
    std::fwrite(bytes.data(), 1, bytes.size(), f);
  }
  std::fclose(f);
  std::printf("  %-40s %zu bytes\n", path.string().c_str(), bytes.size());
}

// Frame a (type, payload) pair into wire bytes the way the CYD (TF_MASTER) sends them.
std::vector<uint8_t> frame(uint8_t type, const uint8_t *payload, size_t len) {
  CaptureTransport t;
  NullHandler nh;
  protocol::FrameLink link(t, TF_MASTER, nh);
  link.send(type, payload, len);
  return t.buf;
}

// --- on-contract payloads, built with the real encoder ---

std::vector<uint8_t> encodeHello() {
  oven_Hello m = oven_Hello_init_zero;
  m.proto_ver = protocol::kProtoVer;
  m.schema_hash = protocol::kSchemaHash; // the exact value the handshake gate demands
  m.boot_nonce = 0xABCD1234;
  uint8_t buf[oven_Hello_size];
  size_t len = 0;
  protocol::encode(oven_Hello_fields, &m, buf, sizeof(buf), len);
  return {buf, buf + len};
}

std::vector<uint8_t> encodeRecipe() {
  oven_Recipe m = oven_Recipe_init_zero;
  m.id = 1;
  m.mode = oven_Mode_MODE_CURE;
  m.seq = 1;
  m.segments_count = 1;
  m.segments[0].dur_ms = 1000;
  m.segments[0].heat_c = 100.0F;
  m.segments[0].interp = oven_Interp_INTERP_HOLD;
  uint8_t buf[oven_Recipe_size];
  size_t len = 0;
  protocol::encode(oven_Recipe_fields, &m, buf, sizeof(buf), len);
  return {buf, buf + len};
}

// A recipe whose mode tag is a value the enum never defines (what an untrusted CYD could
// send; nanopb stores it raw). Seeds the regression for the invalid-enum-load a fuzzer found.
std::vector<uint8_t> encodeRecipeBogusMode() {
  oven_Recipe m = oven_Recipe_init_zero;
  m.id = 2;
  m.mode = static_cast<oven_Mode>(120);
  m.seq = 4;
  m.segments_count = 1;
  m.segments[0].dur_ms = 1000;
  m.segments[0].heat_c = 100.0F;
  m.segments[0].interp = oven_Interp_INTERP_HOLD;
  uint8_t buf[oven_Recipe_size];
  size_t len = 0;
  protocol::encode(oven_Recipe_fields, &m, buf, sizeof(buf), len);
  return {buf, buf + len};
}

std::vector<uint8_t> encodeStart() {
  oven_Start m = oven_Start_init_zero;
  m.session = 0x1234;
  m.recipe_id = 1;
  m.seq = 2;
  uint8_t buf[oven_Start_size];
  size_t len = 0;
  protocol::encode(oven_Start_fields, &m, buf, sizeof(buf), len);
  return {buf, buf + len};
}

std::vector<uint8_t> encodeHeartbeat() {
  oven_Heartbeat m = oven_Heartbeat_init_zero;
  m.session = 0x1234;
  m.seq = 3;
  m.enable = true;
  uint8_t buf[oven_Heartbeat_size];
  size_t len = 0;
  protocol::encode(oven_Heartbeat_fields, &m, buf, sizeof(buf), len);
  return {buf, buf + len};
}

// validator input: [L][recipe..][start..]
std::vector<uint8_t> validatorSeed(const std::vector<uint8_t> &recipe,
                                   const std::vector<uint8_t> &start) {
  std::vector<uint8_t> out;
  out.push_back(static_cast<uint8_t>(recipe.size()));
  out.insert(out.end(), recipe.begin(), recipe.end());
  out.insert(out.end(), start.begin(), start.end());
  return out;
}

// decode input: [type][payload..]
std::vector<uint8_t> decodeSeed(uint8_t type, const std::vector<uint8_t> &payload) {
  std::vector<uint8_t> out;
  out.push_back(type);
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

// --- compiler harness format (fuzz_compiler.cpp): header + 17-byte phase records ---

void putF32(std::vector<uint8_t> &v, float f) {
  uint8_t b[sizeof f];
  std::memcpy(b, &f, sizeof f);
  v.insert(v.end(), b, b + sizeof f);
}

void putU32LE(std::vector<uint8_t> &v, uint32_t x) {
  v.push_back(static_cast<uint8_t>(x));
  v.push_back(static_cast<uint8_t>(x >> 8));
  v.push_back(static_cast<uint8_t>(x >> 16));
  v.push_back(static_cast<uint8_t>(x >> 24));
}

void putU16LE(std::vector<uint8_t> &v, uint16_t x) {
  v.push_back(static_cast<uint8_t>(x));
  v.push_back(static_cast<uint8_t>(x >> 8));
}

// flags: bit0 uv, bit1 motor, bits2-3 convFan{0,1,2} (bits4-5 reserved — no cool fan, §6).
void putPhase(std::vector<uint8_t> &v, float target, float ramp, float hold, float exposure,
              uint8_t flags) {
  putF32(v, target);
  putF32(v, ramp);
  putF32(v, hold);
  putF32(v, exposure);
  v.push_back(flags);
}

// A calibrated two-phase reflow profile (ramp-over-time + hold, then ASAP + hold) — the same shape
// as the recipe_compiler unit test's happy path, so the seed lands squarely in accept territory.
std::vector<uint8_t> compilerReflowSeed() {
  std::vector<uint8_t> v;
  v.push_back(0);   // mode: Reflow
  v.push_back(1);   // model: calibrated preset
  putF32(v, 22.0F); // ambientC
  v.push_back(2);   // phase count
  putPhase(v, 100.0F, 80.0F, 60.0F, 0.0F, 0x00);
  putPhase(v, 150.0F, 0.0F, 30.0F, 0.0F, 0x00);
  return v;
}

// An uncalibrated cure phase with UV + turntable: exercises the exposure→hold estimate path and the
// heuristic fan resolution. flags 0x03 = uv|motor, both fans Auto.
std::vector<uint8_t> compilerCureSeed() {
  std::vector<uint8_t> v;
  v.push_back(1);   // mode: Cure
  v.push_back(0);   // model: uncalibrated (oven_cal::kDefaultModel)
  putF32(v, 22.0F); // ambientC
  v.push_back(1);   // phase count
  putPhase(v, 80.0F, 0.0F, 0.0F, 30.0F, 0x03);
  return v;
}

// --- executor harness format (fuzz_executor.cpp): header + 10-byte segments + 6-byte traj ---

// interp codes match oven_Interp; the harness takes rec[0] % 5, so pass the raw enum values.
void putExecSeg(std::vector<uint8_t> &v, uint8_t interp, float heatC, uint32_t durMs,
                uint8_t channels) {
  v.push_back(interp);
  putF32(v, heatC);
  putU32LE(v, durMs);
  v.push_back(channels);
}

void putExecTraj(std::vector<uint8_t> &v, float tempC, uint16_t stepBias) {
  putF32(v, tempC);
  putU16LE(v, stepBias); // the harness ticks by (1000 + stepBias) ms
}

// A cure-style (ungated) hold that completes on the time-based dose timer.
std::vector<uint8_t> executorCompleteSeed() {
  std::vector<uint8_t> v;
  v.push_back(0x00); // flags: ungated
  v.push_back(1);    // one segment
  putExecSeg(v, /*interp=HOLD*/ 3, /*heatC=*/80.0f, /*durMs=*/3000, /*channels=*/0x00);
  for (int i = 0; i < 6; ++i) {
    putExecTraj(v, 80.0f, 0); // 6 x ~1 s ticks > 3 s hold -> DONE
  }
  return v;
}

// A RAMP_ASAP toward a target the flat trajectory never reaches -> the per-segment watchdog
// must trip TARGET_UNREACHABLE. Seeds the liveness path directly.
std::vector<uint8_t> executorStallSeed() {
  std::vector<uint8_t> v;
  v.push_back(0x00); // flags: ungated (watchdog fires on ASAP regardless)
  v.push_back(1);    // one segment
  putExecSeg(v, /*interp=RAMP_ASAP*/ 2, /*heatC=*/200.0f, /*durMs=*/10000, /*channels=*/0x00);
  for (int i = 0; i < 4; ++i) {
    putExecTraj(v, 25.0f, 0); // stays cold; the harness keeps ticking until the watchdog trips
  }
  return v;
}

// --- heater-control harness format (fuzz_heater_control.cpp): 20-byte gain header + 15-byte ticks
// ---

void putHeaterHeader(std::vector<uint8_t> &v, float kp, float ki, float kd, float tauS,
                     float dutyMax) {
  putF32(v, kp);
  putF32(v, ki);
  putF32(v, kd);
  putF32(v, tauS);
  putF32(v, dutyMax);
}

// flags bit0 = reset() before this tick.
void putHeaterTick(std::vector<uint8_t> &v, float setpointC, float measuredC, float ffDuty,
                   uint16_t stepMs, uint8_t flags) {
  putF32(v, setpointC);
  putF32(v, measuredC);
  putF32(v, ffDuty);
  putU16LE(v, stepMs);
  v.push_back(flags);
}

// A well-behaved feedforward-assisted approach to a 100 °C setpoint, then a reset — lands the seed
// in the ordinary control regime the fuzzer mutates outward from.
std::vector<uint8_t> heaterHoldSeed() {
  std::vector<uint8_t> v;
  putHeaterHeader(v, /*kp=*/0.02f, /*ki=*/0.002f, /*kd=*/0.0f, /*tauS=*/0.0f, /*dutyMax=*/1.0f);
  const float meas[] = {25.0f, 50.0f, 75.0f, 90.0f, 100.0f, 100.0f};
  for (float m : meas) {
    putHeaterTick(v, /*setpoint=*/100.0f, m, /*ff=*/0.75f, /*stepMs=*/1000, /*flags=*/0x00);
  }
  putHeaterTick(v, 100.0f, 100.0f, 0.75f, 1000, /*flags=*/0x01); // exercise reset()
  return v;
}

// --- setpoint-shaper harness format (fuzz_setpoint_shaper.cpp): 40-byte model/config header +
// 11-byte ticks ---

void putShaperHeader(std::vector<uint8_t> &v, float slopeOff, float interceptOff, float floorC,
                     float ceilC, float slopeOn, float interceptOn, float dutySlope, float rateGain,
                     float tauS, float maxLeadC) {
  putF32(v, slopeOff);
  putF32(v, interceptOff);
  putF32(v, floorC);
  putF32(v, ceilC);
  putF32(v, slopeOn);
  putF32(v, interceptOn);
  putF32(v, dutySlope);
  putF32(v, rateGain);
  putF32(v, tauS);
  putF32(v, maxLeadC);
}

// flags bit0 = reset() before this tick, bit1 = convection fan on.
void putShaperTick(std::vector<uint8_t> &v, float spExec, float measuredC, uint16_t stepMs,
                   uint8_t flags) {
  putF32(v, spExec);
  putF32(v, measuredC);
  putU16LE(v, stepMs);
  v.push_back(flags);
}

// An ASAP ramp to 60 °C with the plant trailing the reference — the bench case the shaper exists
// for, so the fuzzer mutates outward from the real control regime rather than from noise. Model
// numbers mirror oven_cal's physics-anchored linearization.
std::vector<uint8_t> shaperRampSeed() {
  std::vector<uint8_t> v;
  putShaperHeader(v, /*slopeOff=*/-0.00197f, /*interceptOff=*/0.549f, /*floor=*/0.01f,
                  /*ceiling=*/2.0f, /*slopeOn=*/-0.00197f, /*interceptOn=*/0.70f,
                  /*dutySlope=*/0.00393f, /*rateGain=*/0.5f, /*tauS=*/50.0f, /*maxLeadC=*/40.0f);
  const float meas[] = {25.0f, 27.0f, 31.0f, 38.0f, 46.0f, 53.0f, 57.0f, 59.0f, 60.0f, 60.0f};
  for (float m : meas) {
    putShaperTick(v, /*spExec=*/60.0f, m, /*stepMs=*/500, /*flags=*/0x02); // fan on (a cure)
  }
  putShaperTick(v, 60.0f, 60.0f, 500, /*flags=*/0x03); // exercise reset()
  return v;
}

// A stalled plant under a falling-then-rising setpoint: seeds the lead clamp and the
// pass-through-on-descent path, the two branches the ramp seed never reaches.
std::vector<uint8_t> shaperClampSeed() {
  std::vector<uint8_t> v;
  putShaperHeader(v, 0.0f, 0.5f, 0.01f, 2.0f, 0.0f, 1.0f, 0.004f, 0.5f, 50.0f, 10.0f);
  for (int i = 0; i < 8; ++i) {
    putShaperTick(v, /*spExec=*/200.0f, /*measuredC=*/25.0f, 1000, 0x00); // never keeps up
  }
  putShaperTick(v, /*spExec=*/40.0f, /*measuredC=*/25.0f, 1000, 0x00); // descent: passes through
  putShaperTick(v, /*spExec=*/200.0f, /*measuredC=*/25.0f, 0, 0x00);   // zero-dt tick
  return v;
}

// A faulted control channel (NaN measurement) with an active derivative gain — seeds the fail-safe
// path and the D seam together.
std::vector<uint8_t> heaterFaultSeed() {
  std::vector<uint8_t> v;
  putHeaterHeader(v, 0.05f, 0.005f, 0.1f, 2.0f, 1.0f);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  putHeaterTick(v, 100.0f, 80.0f, 0.0f, 1000, 0x00);
  putHeaterTick(v, 100.0f, nan, 0.0f, 1000, 0x00);   // blind control → OFF
  putHeaterTick(v, 100.0f, 85.0f, 0.0f, 1000, 0x00); // recovers; D term sees the jump
  return v;
}

// --- safety-supervisor harness format (fuzz_safety_supervisor.cpp): header + 5-byte segments
// + 11-byte driver records ---

// Only the fields armRun reads: dur_ms (runtime budget) and uv/motor (deriveMode cap select).
void putSafetySeg(std::vector<uint8_t> &v, uint32_t durMs, uint8_t channels) {
  putU32LE(v, durMs);
  v.push_back(channels);
}

// ctl bits: bit0 refresh-auth, bit1 trip, bit2 clearFault, bit3 disarm, bit4 fault every wall.
void putSafetyDriver(std::vector<uint8_t> &v, float wallC, float duty, uint16_t stepMs,
                     uint8_t ctl) {
  putF32(v, wallC);
  putF32(v, duty);
  putU16LE(v, stepMs);
  v.push_back(ctl);
}

// A steady, authorized reflow run that never trips: the ordinary regime the fuzzer mutates from.
std::vector<uint8_t> safetyRunSeed() {
  std::vector<uint8_t> v;
  v.push_back(0);                          // flags
  v.push_back(1);                          // one segment
  putSafetySeg(v, /*durMs=*/600000, 0x00); // reflow (no uv/motor)
  for (int i = 0; i < 4; ++i) {
    putSafetyDriver(v, /*wallC=*/100.0f, /*duty=*/0.5f, /*stepMs=*/1000, /*ctl=refresh*/ 0x01);
  }
  return v;
}

// A cure run (uv → 120 °C cap) driven past the over-temp trip point: seeds the L3 trip path.
std::vector<uint8_t> safetyOvertempSeed() {
  std::vector<uint8_t> v;
  v.push_back(0);                          // flags
  v.push_back(1);                          // one segment
  putSafetySeg(v, /*durMs=*/600000, 0x01); // cure (uv on) -> trip above ~135 C
  putSafetyDriver(v, /*wallC=*/200.0f, /*duty=*/0.0f, /*stepMs=*/1000, /*ctl=refresh*/ 0x01);
  return v;
}

// --- profile_store harness format (fuzz_profile_store.cpp): flags + count + name + 17-byte records
//     (the phase records reuse putPhase, same as the compiler harness). ---

void putProfileHeader(std::vector<uint8_t> &v, uint8_t flags, uint8_t phaseCount,
                      const char *name) {
  v.push_back(flags);
  v.push_back(phaseCount);
  const uint8_t nameLen = static_cast<uint8_t>(std::strlen(name));
  v.push_back(nameLen);
  for (const char *c = name; *c != '\0'; ++c) {
    v.push_back(static_cast<uint8_t>(*c));
  }
}

// A two-phase reflow profile that round-trips and compiles cleanly (lands in the save/load +
// compile success path). flags 0x00 = Reflow, not stock; both phases channels-off.
std::vector<uint8_t> profileStoreReflowSeed() {
  std::vector<uint8_t> v;
  putProfileHeader(v, /*flags=*/0x00, /*phaseCount=*/2, "LF-245");
  putPhase(v, 150.0F, 80.0F, 60.0F, 0.0F, 0x00);
  putPhase(v, 245.0F, 0.0F, 30.0F, 0.0F, 0x00);
  return v;
}

// A one-phase cure stock profile with UV + turntable (exercises the stock flag and the cure
// exposure→hold compile path). flags 0x03 = Cure | stock; phase flags 0x03 = uv | motor.
std::vector<uint8_t> profileStoreCureSeed() {
  std::vector<uint8_t> v;
  putProfileHeader(v, /*flags=*/0x03, /*phaseCount=*/1, "cure-30");
  putPhase(v, 80.0F, 0.0F, 0.0F, 30.0F, 0x03);
  return v;
}

} // namespace

int main(int argc, char **argv) {
  const fs::path base = (argc > 1) ? fs::path(argv[1]) : fs::path("fuzz/corpus");
  for (const char *sub :
       {"frontdoor", "decode", "validator", "compiler", "executor", "heater_control",
        "setpoint_shaper", "safety_supervisor", "profile_store"}) {
    fs::create_directories(base / sub);
  }

  const std::vector<uint8_t> hello = encodeHello();
  const std::vector<uint8_t> recipe = encodeRecipe();
  const std::vector<uint8_t> start = encodeStart();
  const std::vector<uint8_t> heartbeat = encodeHeartbeat();

  std::printf("seed corpus -> %s\n", base.string().c_str());

  // frontdoor: full frames as they arrive on the controller's wire.
  writeFile(base / "frontdoor" / "hello.bin",
            frame(protocol::kTfTypeHello, hello.data(), hello.size()));
  writeFile(base / "frontdoor" / "recipe.bin",
            frame(protocol::kTfTypeRecipe, recipe.data(), recipe.size()));
  writeFile(base / "frontdoor" / "start.bin",
            frame(protocol::kTfTypeStart, start.data(), start.size()));
  writeFile(base / "frontdoor" / "heartbeat.bin",
            frame(protocol::kTfTypeHeartbeat, heartbeat.data(), heartbeat.size()));
  writeFile(base / "frontdoor" / "abort.bin", frame(protocol::kTfTypeAbort, nullptr, 0));
  // Prime the parser's overflow/resync paths: a long run of SOF+noise, longer than
  // TinyFrame's 1024-byte RX buffer so the discard_data path is one mutation away.
  writeFile(base / "frontdoor" / "overlong.bin", std::vector<uint8_t>(1300, 0x01));

  // decode: type byte + payload, straight to nanopb.
  writeFile(base / "decode" / "hello.bin", decodeSeed(protocol::kTfTypeHello, hello));
  writeFile(base / "decode" / "recipe.bin", decodeSeed(protocol::kTfTypeRecipe, recipe));
  writeFile(base / "decode" / "start.bin", decodeSeed(protocol::kTfTypeStart, start));
  writeFile(base / "decode" / "heartbeat.bin", decodeSeed(protocol::kTfTypeHeartbeat, heartbeat));

  // validator: length-prefixed Recipe + Start.
  writeFile(base / "validator" / "recipe_start.bin", validatorSeed(recipe, start));
  writeFile(base / "validator" / "bogus_mode.bin", validatorSeed(encodeRecipeBogusMode(), start));

  // compiler: the recipe-compiler harness's struct format (hand-packed, not a wire encoding).
  writeFile(base / "compiler" / "reflow.bin", compilerReflowSeed());
  writeFile(base / "compiler" / "cure.bin", compilerCureSeed());

  // executor: the profile-executor harness's struct format (recipe + trajectory, hand-packed).
  writeFile(base / "executor" / "complete.bin", executorCompleteSeed());
  writeFile(base / "executor" / "stall.bin", executorStallSeed());

  // heater_control: the PI-loop harness's struct format (gain header + tick records, hand-packed).
  writeFile(base / "heater_control" / "hold.bin", heaterHoldSeed());
  writeFile(base / "heater_control" / "fault.bin", heaterFaultSeed());

  // setpoint_shaper: the reference-trajectory harness's format (model/config header + ticks).
  writeFile(base / "setpoint_shaper" / "ramp.bin", shaperRampSeed());
  writeFile(base / "setpoint_shaper" / "clamp.bin", shaperClampSeed());

  // safety_supervisor: the L3-gate harness's struct format (recipe + driver, hand-packed).
  writeFile(base / "safety_supervisor" / "run.bin", safetyRunSeed());
  writeFile(base / "safety_supervisor" / "overtemp.bin", safetyOvertempSeed());

  // profile_store: the ProfileStore harness's struct format (header + name + phase records).
  writeFile(base / "profile_store" / "reflow.bin", profileStoreReflowSeed());
  writeFile(base / "profile_store" / "cure.bin", profileStoreCureSeed());

  std::printf("done\n");
  return 0;
}
