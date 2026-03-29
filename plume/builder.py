"""Build executor: runs package Makefile stages and installs to sysroot."""

import os
import shutil
import subprocess
import sys
import time

from plume.binpkg import binpkg_exists, binpkg_path, create_binpkg, extract_binpkg
from plume.config import Config
from plume.manifest import (
    generate_manifest, check_conflicts, save_installed_manifest,
    installed_manifest_path, read_manifest,
)
from plume.output import green, red, yellow, dim, fmt_duration
from plume.package import Package
from plume.env import get_build_env
from plume.stamp import STAMP_NAME, is_stale, update as update_stamp
from plume.world import World


STAGES = [
    ("pkg_get_source", "Get Source Files"),
    ("pkg_configure",  "Configure"),
    ("pkg_build",      "Build"),
    ("pkg_install",    "Install"),
]


def is_built(config: Config, package: Package) -> bool:
    """Check whether a package has already been built."""
    env = get_build_env(config, package)
    d = env["D"]

    # Live-source packages use a stamp file to detect source changes.
    if package.supports_live_sources and package.live_source_path:
        has_output = os.path.isdir(d) and bool(os.listdir(d))
        stamp_path = os.path.join(env["WORKDIR"], STAMP_NAME)
        return has_output and not is_stale(env["LIVE_SOURCES"], stamp_path)

    if os.path.isdir(d) and bool(os.listdir(d)):
        return True
    # Build tools install to TOOL_INSTALL, not $D
    if package.is_build_tool:
        tool_dir = env.get("TOOL_INSTALL", "")
        pkg_tool_dir = os.path.join(tool_dir, package.name)
        return os.path.isdir(pkg_tool_dir) and bool(os.listdir(pkg_tool_dir))
    return False


def is_installed(config: Config, package: Package) -> bool:
    """Check whether a package is built and installed into the sysroot."""
    if not is_built(config, package):
        return False
    if package.is_build_tool:
        return True  # build tools don't go in world
    world = World(config.get("sysroot"))
    return world.contains(package.qualified_name)


def clean_package(config: Config, package: Package):
    """Remove a package's working directory so it will be fully rebuilt."""
    env = get_build_env(config, package)
    workdir = env["WORKDIR"]
    if os.path.exists(workdir):
        shutil.rmtree(workdir)


def build_package(config: Config, package: Package, verbose: bool = False, force: bool = False) -> tuple[bool, float]:
    """Build a single package by running its Makefile stages.

    This only builds -- it does NOT install into the sysroot.
    Returns (success, elapsed_seconds).
    """
    env = get_build_env(config, package)

    if force and os.path.isdir(env["D"]):
        shutil.rmtree(env["D"])

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

    # Update stamp so future builds can detect source changes.
    if package.supports_live_sources and package.live_source_path:
        update_stamp(os.path.join(env["WORKDIR"], STAMP_NAME))

    # Generate manifest and cache a binary package (skip build tools with no $D)
    if os.path.isdir(env["D"]) and os.listdir(env["D"]):
        manifest = generate_manifest(package, env["D"])
        create_binpkg(config, package, env["D"], manifest)

    return True, time.monotonic() - pkg_start


def install_package(
    config: Config, package: Package,
    verbose: bool = False, force: bool = False,
    no_binary: bool = False,
) -> tuple[bool, float]:
    """Install a package into the sysroot. Builds first if needed.

    If a cached binary package exists (and no_binary is False), it is
    extracted directly into the sysroot, skipping the build.  Otherwise
    the package is built from source (which also creates a binary cache
    entry).  Returns (success, elapsed_seconds).
    """
    total_start = time.monotonic()
    env = get_build_env(config, package)
    sysroot = env["SYSROOT"]

    # Try binary package cache first
    use_binpkg = (
        not no_binary
        and not force
        and not package.is_build_tool
        and is_built(config, package)
        and binpkg_exists(config, package)
    )

    if use_binpkg:
        archive = binpkg_path(config, package)
        manifest = extract_binpkg(archive, sysroot)
        save_installed_manifest(manifest, sysroot)
        world = World(sysroot)
        world.add(package.qualified_name)
        return True, time.monotonic() - total_start

    # Build from source if needed
    if force or not is_built(config, package):
        ok, _ = build_package(config, package, verbose, force=force)
        if not ok:
            return False, time.monotonic() - total_start

    # Copy $D contents into sysroot (skip for build tools with no $D output)
    if os.path.isdir(env["D"]) and os.listdir(env["D"]):
        # Check for file conflicts
        manifest = generate_manifest(package, env["D"])
        conflicts = check_conflicts(manifest, sysroot, exclude_pkg=package.qualified_name)
        if conflicts:
            for path, owner in conflicts:
                print(f"  {yellow('conflict')}: {path} (owned by {owner})")

        _copy_tree(env["D"], sysroot)
        save_installed_manifest(manifest, sysroot)

    # Update world file (build tools don't live in the sysroot)
    if not package.is_build_tool:
        world = World(sysroot)
        world.add(package.qualified_name)

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


def uninstall_package(config: Config, package: Package) -> bool:
    """Remove a package from the sysroot using its installed manifest.

    Deletes all files listed in the manifest, removes empty directories,
    and updates the world file.  Returns True on success.
    """
    sysroot = config.get("sysroot")
    manifest_path = installed_manifest_path(sysroot, package.qualified_name)

    if not os.path.isfile(manifest_path):
        print(f"  {red('error')}: no manifest for {package.qualified_name}, cannot uninstall", file=sys.stderr)
        return False

    manifest = read_manifest(manifest_path)

    # Remove files
    removed_dirs: set[str] = set()
    for entry in manifest.get("files", []):
        fpath = os.path.join(sysroot, entry["path"])
        if os.path.isfile(fpath):
            os.remove(fpath)
            removed_dirs.add(os.path.dirname(fpath))

    # Walk directories bottom-up and remove empty ones
    for d in sorted(removed_dirs, key=len, reverse=True):
        while d != sysroot and os.path.isdir(d) and not os.listdir(d):
            os.rmdir(d)
            d = os.path.dirname(d)

    # Remove manifest and world entry
    os.remove(manifest_path)
    world = World(sysroot)
    world.remove(package.qualified_name)

    return True


def _copy_tree(src: str, dst: str):
    """Recursively copy src tree into dst, merging directories."""
    for root, dirs, files in os.walk(src):
        rel = os.path.relpath(root, src)
        dst_dir = os.path.join(dst, rel)
        os.makedirs(dst_dir, exist_ok=True)
        for f in files:
            shutil.copy2(os.path.join(root, f), os.path.join(dst_dir, f))
