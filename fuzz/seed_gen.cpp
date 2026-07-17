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
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
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

// flags: bit0 uv, bit1 motor, bits2-3 convFan{0,1,2}, bits4-5 coolFan{0,1,2}.
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
  v.push_back(0);   // model: uncalibrated (oven_cal::DEFAULT)
  putF32(v, 22.0F); // ambientC
  v.push_back(1);   // phase count
  putPhase(v, 80.0F, 0.0F, 0.0F, 30.0F, 0x03);
  return v;
}

} // namespace

int main(int argc, char **argv) {
  const fs::path base = (argc > 1) ? fs::path(argv[1]) : fs::path("fuzz/corpus");
  for (const char *sub : {"frontdoor", "decode", "validator", "compiler"}) {
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

  std::printf("done\n");
  return 0;
}
