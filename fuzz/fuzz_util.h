// Shared primitives for every libFuzzer harness in this directory (fuzz/README.md) —
// the untrusted-input pipeline harnesses AND the internal-property / differential ones.
// It deliberately holds nothing pipeline-specific: the controller RX scaffolding lives in
// fuzz_pipeline.h, so a harness that fuzzes pure logic (e.g. fuzz_compiler) doesn't drag
// in TinyFrame/router/link it never touches.
//
// Host-only, clang-only: this header pulls <fuzzer/FuzzedDataProvider.h> from the
// (mise-pinned) clang toolchain, so only the fuzz_* harness envs include it. The seed
// generator (seed_gen.cpp), which builds under plain GCC, must NOT include it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <fuzzer/FuzzedDataProvider.h> // available to harnesses that want structured carving

// Always-abort invariant check. Deliberately not <cassert>'s assert: a release-mode
// -DNDEBUG build would compile that to nothing, silently disarming every semantic
// invariant. A fuzzer with disarmed postconditions finds only crashes.
#define FUZZ_ASSERT(cond)                                                                          \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::fprintf(stderr, "FUZZ_ASSERT failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);         \
      std::abort();                                                                                \
    }                                                                                              \
  } while (0)
