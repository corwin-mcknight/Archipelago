#!/usr/bin/env python3
"""Merge per-translation-unit -MJ JSON fragments into compile_commands.json.

Each clang -MJ <file> invocation writes a single JSON object followed by a
trailing comma (e.g. `{ ... },`).  This script collects all such fragments
from the build tree, strips the trailing commas, filters stale entries whose
source file no longer exists, and emits a well-formed JSON array for clangd.

Usage:
    python3 tools/merge-compile-commands.py <build_dir> <output_file>

Example:
    python3 tools/merge-compile-commands.py build build/compile_commands.json
"""

import glob
import json
import os
import sys


def is_compile_entry(entry: object) -> bool:
    """Return True if entry looks like a compile_commands.json record."""
    # clang -MJ emits "arguments" (array); bear emits "command" (string); accept both.
    return isinstance(entry, dict) and ("command" in entry or "arguments" in entry) and "file" in entry


def source_exists(entry: dict) -> bool:
    """Return True if the source file referenced by the entry exists on disk."""
    file_path = entry.get("file", "")
    if not file_path:
        return False
    if os.path.isabs(file_path):
        return os.path.exists(file_path)
    # Relative paths are resolved against the "directory" field.
    directory = entry.get("directory", "")
    return os.path.exists(os.path.join(directory, file_path))


def main() -> None:
    build_dir = sys.argv[1] if len(sys.argv) > 1 else "build"
    output = sys.argv[2] if len(sys.argv) > 2 else os.path.join(build_dir, "compile_commands.json")

    fragments = glob.glob(os.path.join(build_dir, "**/*.json"), recursive=True)

    entries: list[dict] = []
    stale = 0
    for path in sorted(fragments):
        if os.path.abspath(path) == os.path.abspath(output):
            continue
        try:
            text = open(path).read().strip().rstrip(",")
            entry = json.loads(text)
            if not is_compile_entry(entry):
                continue
            if not source_exists(entry):
                stale += 1
                continue
            entries.append(entry)
        except Exception as exc:
            print(f"warning: skipping {path}: {exc}", file=sys.stderr)

    with open(output, "w") as f:
        json.dump(entries, f, indent=2)
        f.write("\n")

    msg = f"compile_commands.json: {len(entries)} entries → {output}"
    if stale:
        msg += f" ({stale} stale fragment(s) skipped)"
    print(msg)


if __name__ == "__main__":
    main()
