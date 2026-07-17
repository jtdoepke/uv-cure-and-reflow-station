"""PlatformIO extra-script for the fuzz_* envs: build with clang + libFuzzer.

libFuzzer is clang-only (GCC has no -fsanitize=fuzzer), so this swaps the native
platform's default GCC toolchain for the mise-pinned clang (mise.toml [tools]) and
adds -fsanitize=fuzzer,address,undefined to BOTH the compile and link steps — the
native builder does not forward -fsanitize to the linker on its own. The linked
program IS the libFuzzer binary (libFuzzer supplies main); run .pio/build/<env>/program
directly with libFuzzer args.

Runs as a `post:` script and patches EVERY build environment — the base env, the
project-sources env (projenv), and each library builder's env — because the native
platform (platforms/native/builder/main.py) hardcodes gcc/g++ via env.Tool() after
`pre:` scripts run, and those envs are independent clones. Every object under test
(TinyFrame, nanopb, the protocol/control_logic libs, the harness) must be clang +
-fsanitize=fuzzer-instrumented, or libFuzzer sees no coverage inside them and ASAN
misses their bugs. SCons expands $CC/$CXX at build time, so a post: replace still wins.

Toolchain quirks this handles (see mise.toml's clang comment):
  * The vfox-clang (pixi) prebuilt exposes only a `clang` driver, no `clang++`, so C++
    compiles and the link go through `clang --driver-mode=g++`.
  * That clang is not wired to a C++ stdlib, so `--gcc-toolchain=<prefix>` points it at
    the system GCC's libstdc++ (a system GCC is thus a fuzz prerequisite).

clang is resolved PATH-first, then via `mise which clang`, so a bare (non-activated)
shell finds it too — mirroring tools/schema_hash_extra_script.py's protoc lookup.
"""

import os
import shutil
import subprocess

Import("env")  # noqa: F821 — provided by the PlatformIO/SCons runtime


def _resolve_clang():
    """The clang driver: activated-mise PATH first, then `mise which clang`."""
    found = shutil.which("clang")
    if found:
        return found
    mise = shutil.which("mise")
    if mise:
        probe = subprocess.run([mise, "which", "clang"], capture_output=True, text=True)
        if probe.returncode == 0 and probe.stdout.strip():
            return probe.stdout.strip()
    raise SystemExit(
        "fuzz: clang not found — run `mise install` (clang is pinned in mise.toml for `make fuzz`)"
    )


def _gcc_prefix():
    """Prefix of the system GCC whose libstdc++ the prebuilt clang should use."""
    gcc = shutil.which("gcc") or shutil.which("g++")
    if not gcc:
        raise SystemExit("fuzz: no system gcc/g++ found — clang needs its libstdc++ (--gcc-toolchain)")
    return os.path.dirname(os.path.dirname(os.path.realpath(gcc)))


clang = _resolve_clang()
common = ["-fsanitize=fuzzer,address,undefined", f"--gcc-toolchain={_gcc_prefix()}"]
cxx_mode = ["--driver-mode=g++"]  # this clang ships no clang++ driver

# Every independent build environment: base, project sources, and each library.
build_envs = [env]
try:
    Import("projenv")  # noqa: F821 — project-sources env; only defined this late in the build
    build_envs.append(projenv)  # noqa: F821
except Exception:  # pragma: no cover — projenv absent (e.g. nothing to build)
    pass
build_envs.extend(builder.env for builder in env.GetLibBuilders())

for e in build_envs:
    e.Replace(CC=clang, CXX=clang, LINK=clang)
    e.Append(CCFLAGS=common, CXXFLAGS=cxx_mode, LINKFLAGS=common + cxx_mode)
