#!/usr/bin/env python3
"""Produce a clang-tidy-compatible copy of the esp32dev_cyd compile_commands.json.

clang-tidy is clang-based, so the Xtensa GCC entries in the firmware DB fail for the same
reasons documented in .clangd — but unlike clangd, clang-tidy doesn't read .clangd. This
script applies the same transformation .clangd describes, directly to a copy of the DB:
strip the Xtensa-only GCC flags clang rejects, add `--target=xtensa -D__XTENSA__=1` (the
clang-tidy PyPI wheel ships the Xtensa target) and `-nostdlibinc` (keep host glibc out;
the toolchain's own headers were already injected as -isystem by
tools/clangd-inject-sysincludes.py via `make compiledb`).

Keep the Remove/Add lists below in sync with .clangd. Run by `make tidy`:

    tools/tidy-sanitize-compiledb.py <input-db> <output-dir>
"""
import fnmatch
import json
import os
import shlex
import sys

# Mirror of .clangd CompileFlags.Remove
REMOVE = {
    "-mlongcalls",
    "-mtext-section-literals",
    "-mdisable-hardware-atomics",
    "-mfix-esp32-psram-cache-issue",
    "-fstrict-volatile-bitfields",
    "-fno-tree-switch-conversion",
    "-freorder-blocks",
}
REMOVE_GLOB = ["-mfix-esp32-psram-cache-strategy=*"]
# Mirror of .clangd CompileFlags.Add
ADD = ["--target=xtensa", "-D__XTENSA__=1", "-nostdlibinc"]


def main():
    if len(sys.argv) != 3:
        sys.exit(f"usage: {sys.argv[0]} <compile_commands.json> <output-dir>")
    src_db, out_dir = sys.argv[1], sys.argv[2]
    if not os.path.exists(src_db):
        sys.exit(f"{src_db} not found — run `make compiledb` first")

    with open(src_db) as f:
        db = json.load(f)

    out = []
    for entry in db:
        args = entry.get("arguments") or shlex.split(entry["command"])
        if "xtensa" not in args[0]:
            continue  # host entries (if any) aren't tidy's business here
        args = [
            a
            for a in args
            if a not in REMOVE and not any(fnmatch.fnmatch(a, g) for g in REMOVE_GLOB)
        ]
        args[1:1] = ADD  # right after the compiler, before -c/the file
        out.append({"directory": entry["directory"], "file": entry["file"], "arguments": args})

    os.makedirs(out_dir, exist_ok=True)
    dest = os.path.join(out_dir, "compile_commands.json")
    with open(dest, "w") as f:
        json.dump(out, f, indent=2)
    print(f"tidy: sanitized {len(out)} Xtensa entries into {dest}")


if __name__ == "__main__":
    main()
