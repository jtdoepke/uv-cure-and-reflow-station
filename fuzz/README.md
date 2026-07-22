# Fuzzing

The repo's coverage-guided [libFuzzer](https://llvm.org/docs/LibFuzzer.html) suite — the
home for property/differential fuzzing of any host-buildable `lib/` code, not just one
subsystem. Harnesses are host-only out-of-tree consumers of `lib/` (same slot as `sim/`
and `perf/`), never under `lib/`, never board-aware. They fall into two families:

**1. Untrusted-input pipeline** — the path every attacker-controllable byte from the CYD
takes through the controller (design.md §4/§9/§10):

```text
raw bytes → FrameLink::poll() → TinyFrame (CRC-16, 1024B RX buf) → MessageRouter
  → protocol::decode (nanopb) → ControllerLink → SetupResponder → RecipeValidator
```

These harnesses share the cold-wired `fuzz::ControllerHarness` in `fuzz_pipeline.h`.

**2. Internal-correctness properties** — differentials and invariants over pure logic
that never sees the wire, but whose output the safety-critical code trusts. Today: the
CYD's recipe compiler (`lib/app_logic`), fuzzed against the controller's `RecipeValidator`
so that **every recipe it green-lights is one the controller accepts** — a producer that
out-runs its own backstop is the bug. These need only the shared `fuzz_util.h`.

## Prerequisites: clang (via mise) + a system GCC

libFuzzer is clang-only (GCC has no `-fsanitize=fuzzer`). clang is pinned in
`mise.toml` — the one heavyweight tool (a prebuilt LLVM toolchain, a couple GB,
mise-cached after the first install):

```sh
mise install          # provisions the pinned clang (first run downloads it)
```

Two things worth knowing (both handled by `tools/fuzz_clang_extra_script.py`):

- The pin uses the **vfox-clang** backend, not the default `clang` backend — the latter
  source-builds LLVM on distros without a matching prebuilt (needs cmake/ninja, hours).
- That prebuilt clang isn't wired to a C++ stdlib and ships only a `clang` driver, so the
  script drives it as `clang --driver-mode=g++ --gcc-toolchain=<sys>`. **A system GCC is
  therefore required** (clang borrows its libstdc++) — every Linux dev box has one.

CI never fuzzes, so its mise step sets `MISE_DISABLE_TOOLS=clang` to skip the download.
clang is resolved PATH-first then via `mise which clang`, so a bare (non-activated) shell
works too.

## Run

```sh
make fuzz                          # build + run every harness, 60s each
make fuzz FUZZ_TARGET=frontdoor    # just one harness
make fuzz FUZZ_TIME=600            # longer session
make fuzz-corpus                   # fast: replay the committed corpus, no mutation (must exit 0)
make fuzz-seed                     # regenerate fuzz/corpus/** (real encoder + hand-packed seeds)
```

Under the hood each env builds `.pio/build/fuzz_<name>/program` — that binary *is* the
libFuzzer executable, so you can drive it directly with any libFuzzer flag:

```sh
pio run -e fuzz_frontdoor
.pio/build/fuzz_frontdoor/program fuzz/corpus/frontdoor -max_len=1200 -max_total_time=120
```

## Reproduce a finding

On a crash/UBSAN/ASAN hit, libFuzzer writes the offending bytes to `crash-<sha>` (or
`timeout-*`, `oom-*`) in the run dir (git-ignored). Replay it:

```sh
.pio/build/fuzz_frontdoor/program crash-<sha>
```

Then add a shrunk copy to the corpus (or a `native_control` regression test) once fixed.

## The harnesses

Untrusted-input pipeline (family 1):

| Harness | Env | Seam |
|---|---|---|
| `fuzz_frontdoor.cpp` | `fuzz_frontdoor` | raw bytes → whole RX pipeline (TinyFrame + nanopb + router + validator). Asserts noise never reaches `authorized()`. |
| `fuzz_decode.cpp` | `fuzz_decode` | `[type][payload]` → `MessageRouter` → nanopb `pb_decode`, skipping the CRC gate for max decode density. |
| `fuzz_validator.cpp` | `fuzz_validator` | `[len][Recipe][Start]` → `RecipeValidator`. Asserts accept-implies-constraints. |

Internal-correctness properties (family 2):

| Harness | Env | Property |
|---|---|---|
| `fuzz_compiler.cpp` | `fuzz_compiler` | phases → `compileRecipe` → the real `RecipeValidator`. Asserts every hard-valid compile is accepted (plus `dur_ms > 0`, `≤ 32` segments, finite setpoints). |
| `fuzz_executor.cpp` | `fuzz_executor` | a raw recipe + an adversarial temperature trajectory → `ProfileExecutor`. Asserts **liveness** (the per-segment watchdog always leaves RUNNING within a bounded tick count), plus setpoint finite/≤ max target, in-range monotonic `segIdx`, and terminal-state safety. |
| `fuzz_setpoint_shaper.cpp` | `fuzz_setpoint_shaper` | an adversarial plant model (inverted/NaN/Inf rate envelopes) + signal trajectory → `SetpointShaper`, the reference the PI tracks (§5). Asserts the reference stays finite and **never exceeds the executor's setpoint** (the "can only reduce commanded heat" claim the run path's safety argument rests on) nor leads the measurement past `maxLeadC`, that the reported rate matches the reference's actual movement (it becomes a feedforward duty bump, so a rate that disagrees commands heat for motion that is not happening), and that blind control passes **through** rather than being laundered into a finite value. Also composes the pair the run path composes, `rampFeedforwardDuty` over the shaper's own output, which is how it found `steadyStateDuty` emitting NaN for a non-finite model (`clampf` passes NaN through). |
| `fuzz_profile_store.cpp` | `fuzz_profile_store` | arbitrary bytes → `ProfileStore::load`/`list` (a profile can be pushed over serial/WiFi, §7, so this is an untrusted deserializer). Asserts a malformed blob is **rejected, never mis-parsed** (any successful load is well-formed: `phaseCount ≤ kMaxPhases`, NUL-terminated name, mode == the store's), a save→load round-trip is byte-faithful, and a loaded profile is a valid `compileRecipe` input (no UB; hard-valid ⇒ the B1 segment invariants). |
| `fuzz_profile_facts.cpp` | `fuzz_profile_facts` | a raw `Phase[]` (adversarial floats) → `profile_facts::computeFacts`/`sampleCurve` (C4's new consumer of an untrusted loaded profile — the store bounds `phaseCount`/name but not the float fields). Asserts the facts + **every** emitted curve point stay finite and within the declared bounds (`peakC`/`T` in `[kTempLo,kTempHi]`, `totalSeconds`/`t` in `[0,kMaxSeconds]`, `≤ kMaxCurvePoints`, `t` monotonic), so no NaN coordinate can reach `lv_line`, and the formatters always NUL-terminate. |

`fuzz/corpus/<name>/` holds committed seeds, generated by `seed_gen.cpp` (env
`fuzz_seedgen`, plain GCC). The pipeline seeds are built with the real encoder/framer so
they are valid wire traffic; the compiler seeds are hand-packed in that harness's own
struct format (it fuzzes a producer, so there is no wire message to encode). Each input
format is documented at the top of its harness and mirrored in `seed_gen.cpp`.

**Every committed seed's filename is prefixed with `_`** (`_hello.bin`, `_idle_ticks`).
That is what `.gitignore` keys on: it ignores everything under `fuzz/corpus/**` except the
`_`-prefixed seeds, so libFuzzer discoveries — written as bare 40-char SHA1 filenames when
a harness is driven directly (the "drive it directly" command above uses the corpus dir as
its writable output) — never show up as untracked and never get committed by accident.
`make fuzz` itself already keeps the corpus pristine (it writes to a scratch dir and passes
the corpus read-only); the prefix guards the direct-invocation path. New hand-dropped seeds
must carry the prefix too, or they won't be tracked.

## Add a fuzz target

Fuzzing new code is mechanical — the run tooling auto-discovers harnesses, so you never
edit the Makefile or CI:

1. **Write** `fuzz/fuzz_<name>.cpp`: `#include "fuzz_util.h"` (for `FUZZ_ASSERT` +
   `FuzzedDataProvider`) — or `#include "fuzz_pipeline.h"` if you want the cold-wired
   controller RX stack — then define `LLVMFuzzerTestOneInput(const uint8_t*, size_t)` and
   carve the input into the shape your code-under-test wants (a documented manual layout,
   as the existing harnesses use, or `FuzzedDataProvider`). Assert **domain invariants**,
   not just "no crash" (`FUZZ_ASSERT`) — the strongest ones are *differentials* against an
   independent oracle (e.g. the compiler vs the validator) — a fuzzer with real
   postconditions finds far more than one that only catches segfaults.
2. **Add** `[env:fuzz_<name>]` to `platformio.ini`: `extends = fuzz_common` plus one
   `build_src_filter = -<*> +<../fuzz/fuzz_<name>.cpp>` line. That is the whole env.
3. **Seed** `fuzz/corpus/<name>/` (extend `seed_gen.cpp`, or drop representative
   inputs). Prefix every seed filename with `_` so `.gitignore` tracks it; commit the seeds.
4. **Run** `make fuzz FUZZ_TARGET=<name>`.

**Which base to `extends`:** `fuzz_common` = nanopb + schema-hash + controller/protocol
libs (`lib_ignore = LovyanGFX lvgl`), and it works for a pure `lib/app_logic` target too
(the `fuzz_compiler` env just extends it unchanged — nanopb is needed for `oven.pb.h`
either way, and `lib_ldf_mode = deep+` pulls in whatever headers the harness reaches).
Only copy `fuzz_common` minus `custom_nanopb_protos` + the schema-hash pre-script if a
target genuinely touches no protobuf. For UI logic, base on `native_ui_cyd`'s LVGL config
plus the clang/fuzzer extra-script. Add a new `*_common` base only when a live harness
uses it — an env nothing builds rots.

`fuzz_util.h` includes `<fuzzer/FuzzedDataProvider.h>` from the clang toolchain (no
vendored copy to drift), so only the clang-built harness envs may include it — the
GCC-built `seed_gen.cpp` must not. `fuzz_pipeline.h` is the untrusted-input-only
scaffolding layered on top; a property harness that never touches the wire skips it.
