"""Build executor: runs package Makefile stages and installs to sysroot."""

import os
import shutil
import subprocess
import sys
import time

from plume.config import Config
from plume.output import green, red, dim, fmt_duration
from plume.package import Package
from plume.env import get_build_env
from plume.world import World


STAGES = [
    ("pkg_get_source", "Get Source Files"),
    ("pkg_configure",  "Configure"),
    ("pkg_build",      "Build"),
    ("pkg_install",    "Install"),
]


def is_built(config: Config, package: Package) -> bool:
    """Check whether a package has already been built (has a populated $D)."""
    env = get_build_env(config, package)
    d = env["D"]
    return os.path.isdir(d) and bool(os.listdir(d))


def build_package(config: Config, package: Package, verbose: bool = False) -> tuple[bool, float]:
    """Build a single package by running its Makefile stages.

    This only builds — it does NOT install into the sysroot.
    Returns (success, elapsed_seconds).
    """
    env = get_build_env(config, package)

    # Create working directories
    for d in [env["WORKDIR"], env["S"], env["D"]]:
        os.makedirs(d, exist_ok=True)

    makefile = os.path.join(env["FILESDIR"], "Makefile")
    if not os.path.exists(makefile):
        print(f"  {red('error')}: no Makefile found at {makefile}", file=sys.stderr)
        return False, 0.0

    pkg_start = time.monotonic()

    for stage, label in STAGES:
        ok, _ = _run_stage(stage, label, makefile, env, verbose)
        if not ok:
            return False, time.monotonic() - pkg_start

    return True, time.monotonic() - pkg_start


def install_package(config: Config, package: Package, verbose: bool = False) -> tuple[bool, float]:
    """Install a package into the sysroot. Builds first if needed.

    Copies the package's $D tree into the sysroot and records it in the
    world file. Build tools are skipped (they install to $TOOL_INSTALL).
    Returns (success, elapsed_seconds).
    """
    total_start = time.monotonic()

    # Build first if the package hasn't been built yet
    if not is_built(config, package):
        ok, _ = build_package(config, package, verbose)
        if not ok:
            return False, time.monotonic() - total_start

    env = get_build_env(config, package)

    # Copy $D contents into sysroot (skip for build tools with no $D output)
    if os.listdir(env["D"]):
        _copy_tree(env["D"], env["SYSROOT"])

    # Update world file (build tools don't live in the sysroot)
    if not package.is_build_tool:
        world = World(env["SYSROOT"])
        world.add(package.full_name)

    return True, time.monotonic() - total_start


def _run_stage(stage: str, label: str, makefile: str, env: dict, verbose: bool) -> tuple[bool, float]:
    """Run a single Make stage, printing a status row. Returns (success, elapsed)."""
    t0 = time.monotonic()

    if verbose:
        result = subprocess.run(
            [env["MAKE"], "-f", makefile, "--no-print-directory", stage],
            env=env,
            cwd=env["S"],
        )
        captured = None
    else:
        result = subprocess.run(
            [env["MAKE"], "-f", makefile, "--no-print-directory", stage],
            env=env,
            cwd=env["S"],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        captured = result.stdout

    elapsed = time.monotonic() - t0
    ok = result.returncode == 0
    row = f"  {label:<22}"

    if ok:
        print(f"{row}  {green('✓')}  {dim(fmt_duration(elapsed))}")
    else:
        print(f"{row}  {red('✗')}  FAILED")
        if captured:
            print(captured, end="")

    return ok, elapsed


def _copy_tree(src: str, dst: str):
    """Recursively copy src tree into dst, merging directories."""
    for root, dirs, files in os.walk(src):
        rel = os.path.relpath(root, src)
        dst_dir = os.path.join(dst, rel)
        os.makedirs(dst_dir, exist_ok=True)
        for f in files:
            shutil.copy2(os.path.join(root, f), os.path.join(dst_dir, f))
