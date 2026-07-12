#!/usr/bin/env python3
"""Bake the ESP32 Xtensa toolchain's builtin system-include paths into compile_commands.json.

clangd is clang-based and can't self-discover the GCC cross-toolchain's system headers
(stdio.h, machine/endian.h, ...). Its --query-driver mechanism is unreliable here because our
.clangd adds clang-only flags (--target=xtensa) that the GCC driver rejects when clangd probes
it. So instead we ask the compiler named in the DB for its `#include <...>` search dirs and add
them to every entry as `-isystem` — making the DB self-contained. Combined with `-nostdlibinc`
in .clangd (which drops clang's host /usr/include), clangd then resolves ESP-IDF/newlib headers
correctly with no host-glibc leak.

compile_commands.json is git-ignored and regenerated per machine, so the absolute toolchain
paths added here never get committed. Run automatically by `make compiledb`.
"""
import json
import os
import shlex
import shutil
import subprocess
import sys

DB = sys.argv[1] if len(sys.argv) > 1 else "compile_commands.json"


def compiler_of(entry):
    if "arguments" in entry:
        return entry["arguments"][0]
    return shlex.split(entry["command"])[0]


def resolve(cc, toolchain_bin):
    """Turn a compiler token (possibly a bare name) into a runnable path."""
    if os.path.isabs(cc) and os.path.exists(cc):
        return cc
    found = shutil.which(cc)
    if found:
        return found
    if toolchain_bin:
        cand = os.path.join(toolchain_bin, os.path.basename(cc))
        if os.path.exists(cand):
            return cand
    return None


def system_include_dirs(compiler):
    """Return the compiler's builtin `#include <...>` search directories."""
    res = subprocess.run(
        [compiler, "-xc++", "-E", "-v", os.devnull],
        capture_output=True,
        text=True,
    )
    dirs, collecting = [], False
    for line in res.stderr.splitlines():
        if "#include <...> search starts here" in line:
            collecting = True
            continue
        if "End of search list" in line:
            break
        if collecting:
            d = line.strip()
            if os.path.isdir(d):
                dirs.append(os.path.normpath(d))
    return dirs


def main():
    if not os.path.exists(DB):
        sys.exit(f"{DB} not found — run `pio run -e esp32dev_cyd -t compiledb` first")
    with open(DB) as f:
        db = json.load(f)
    if not db:
        return

    # Learn the toolchain bin dir from any absolute compiler path, to resolve bare names.
    toolchain_bin = ""
    for entry in db:
        cc = compiler_of(entry)
        if os.path.isabs(cc) and "xtensa" in cc:
            toolchain_bin = os.path.dirname(cc)
            break

    cache = {}
    for entry in db:
        cc = compiler_of(entry)
        # Only touch cross-compiler entries (skip any host g++/clang entries).
        if "xtensa" not in cc:
            continue
        if cc not in cache:
            exe = resolve(cc, toolchain_bin)
            cache[cc] = system_include_dirs(exe) if exe else []
        flags = " ".join("-isystem " + shlex.quote(d) for d in cache[cc])
        if not flags:
            continue
        if "arguments" in entry:
            extra = []
            for d in cache[cc]:
                extra += ["-isystem", d]
            # insert right after the compiler so they precede -c/file
            entry["arguments"][1:1] = extra
        else:
            toks = entry["command"].split(" ", 1)
            entry["command"] = toks[0] + " " + flags + (" " + toks[1] if len(toks) > 1 else "")

    with open(DB, "w") as f:
        json.dump(db, f, indent=2)
    n = len(next(iter(cache.values()))) if cache else 0
    print(f"clangd: injected {n} toolchain system-include dirs into {DB}")


if __name__ == "__main__":
    main()
