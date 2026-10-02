#!/usr/bin/env python3
"""Read-only checks for Archipelago's native development prerequisites."""

import argparse
import importlib.metadata
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent


def check_tool(name, minimum=None):
    path = shutil.which(name)
    if not path:
        print(f"  MISSING {name}")
        return False
    if minimum is not None:
        result = subprocess.run([path, "--version"], text=True, capture_output=True)
        match = re.search(r"(?:clang|LLVM) version (\d+)", result.stdout + result.stderr)
        if result.returncode or not match or int(match.group(1)) < minimum:
            print(f"  FAIL    {name}: LLVM {minimum}+ required ({path})")
            return False
    if name == "make":
        result = subprocess.run([path, "--version"], text=True, capture_output=True)
        if result.returncode or "GNU Make" not in result.stdout:
            print(f"  FAIL    make: GNU Make required ({path})")
            return False
    print(f"  OK      {name}: {path}")
    return True


def check_macos_sdk():
    xcrun = shutil.which("xcrun")
    if not xcrun:
        print("  MISSING xcrun: Apple's Command Line Tools or Xcode is required")
        return False
    try:
        result = subprocess.run([xcrun, "--sdk", "macosx", "--show-sdk-path"], text=True, capture_output=True)
    except OSError as error:
        print(f"  FAIL    macOS SDK: cannot run xcrun ({error})")
        return False
    sdk = result.stdout.strip()
    if result.returncode or not sdk or not Path(sdk).is_dir():
        print("  FAIL    macOS SDK: use a working Xcode or Command Line Tools installation")
        return False
    print(f"  OK      macOS SDK: {sdk}")
    return True


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", choices=("x86_64", "riscv64", "riscv64^jh7110", "all"), default="all")
    args = parser.parse_args(argv)
    print(f"Native environment: {platform.system()} {platform.machine()}")
    failures = 0
    # PEP 604 annotations used by Plume require Python 3.10 or newer.
    if sys.version_info < (3, 10):
        print("  FAIL    Python 3.10+ required")
        failures += 1
    else:
        print(f"  OK      Python {platform.python_version()}: {sys.executable}")
    if Path(sys.prefix).resolve() != (ROOT / ".venv").resolve():
        print("  MISSING checkout-local .venv (run make setup)")
        failures += 1
    try:
        required = next(line.split("==", 1)[1] for line in (ROOT / "requirements.txt").read_text().splitlines()
                        if line.startswith("PyYAML=="))
        installed = importlib.metadata.version("PyYAML")
        if installed != required:
            print(f"  FAIL    PyYAML {installed}; expected {required}")
            failures += 1
        else:
            print(f"  OK      PyYAML {installed}")
    except importlib.metadata.PackageNotFoundError:
        print("  MISSING PyYAML (run make setup)")
        failures += 1

    tools = ["clang", "clang++", "ld.lld", "llvm-ar", "llvm-objcopy", "clangd", "clang-format",
             "llvm-cov", "llvm-profdata", "make", "git", "curl", "xorriso", "ar", "tar"]
    if platform.system() == "Darwin":
        tools.append("lldb")
        failures += not check_macos_sdk()
    if args.arch in ("all", "x86_64"):
        tools += ["nasm", "qemu-system-x86_64"]
    if args.arch in ("all", "riscv64", "riscv64^jh7110"):
        tools += ["qemu-system-riscv64", "mformat", "mcopy", "mmd", "mpartition", "sha256sum", "truncate", "xz"]
    for tool in tools:
        failures += not check_tool(tool, minimum=17 if tool in ("clang", "clang++") else None)
    for tool in ("gdb", "doxygen", "socat"):
        path = shutil.which(tool)
        print(f"  OPTIONAL {tool}: {path or 'not installed'}")
    print(f"\n{'Ready.' if failures == 0 else f'{failures} prerequisite check(s) failed; run make setup.'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
