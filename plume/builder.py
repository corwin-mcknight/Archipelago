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
from plume.output import green, red, yellow, dim, fmt_duration, break_line
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


def build_needed(config: Config, package: Package) -> str | None:
    """Return why the package needs a (re)build, or None if its build is fresh."""
    env = get_build_env(config, package)
    d = env["D"]

    has_output = os.path.isdir(d) and bool(os.listdir(d))
    # Build tools install to TOOL_INSTALL, not $D
    if not has_output and package.is_build_tool:
        pkg_tool_dir = os.path.join(env.get("TOOL_INSTALL", ""), package.name)
        has_output = os.path.isdir(pkg_tool_dir) and bool(os.listdir(pkg_tool_dir))
    if not has_output:
        return "never built"

    stamp_path = os.path.join(env["WORKDIR"], STAMP_NAME)
    source_path = env["LIVE_SOURCES"] if package.supports_live_sources and package.live_source_path else None
    return is_stale(stamp_path, config.build_hash, source_path)


def install_needed(config: Config, package: Package) -> str | None:
    """Return why the package needs an install, or None if its current build is installed.

    World membership alone is insufficient: it survives a rebuild, so a package
    that was reinstalled after editing its sources would look "installed" while
    the sysroot still holds the old files. Require the installed manifest to be
    at least as new as the build stamp, so a rebuild forces a reinstall.
    """
    reason = build_needed(config, package)
    if reason:
        return reason
    if package.is_build_tool:
        return None  # build tools don't go in world
    sysroot = config.get("sysroot")
    if not World(sysroot).contains(package.qualified_name):
        return "not installed"
    stamp_path = os.path.join(get_build_env(config, package)["WORKDIR"], STAMP_NAME)
    manifest_path = installed_manifest_path(sysroot, package.qualified_name)
    if not os.path.exists(manifest_path):
        return "not installed"
    if os.path.getmtime(manifest_path) < os.path.getmtime(stamp_path):
        return "rebuilt since install"
    return None


def build_log_path(config: Config, package: Package) -> str:
    """Path of the persisted build log for a package."""
    return os.path.join(config.get("build_dir"), "logs", package.category, f"{package.name}.log")


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

    for d in [env["WORKDIR"], env["S"], env["D"]]:
        os.makedirs(d, exist_ok=True)

    makefile = os.path.join(env["FILESDIR"], "Makefile")
    if not os.path.exists(makefile):
        break_line()
        print(f"  {red('error')}: no Makefile found at {makefile}", file=sys.stderr)
        return False, 0.0

    pkg_start = time.monotonic()

    log_path = build_log_path(config, package)
    os.makedirs(os.path.dirname(log_path), exist_ok=True)
    with open(log_path, "w", encoding="utf-8") as log:
        if verbose:
            log.write("(output streamed to console; rerun without --verbose to capture)\n")
        for stage, label in STAGES:
            ok, _ = _run_stage(stage, label, makefile, env, verbose, log)
            if not ok:
                print(f"  {dim(f'full log: {log_path}')}")
                return False, time.monotonic() - pkg_start

    # Record the config hash so source or config changes trigger a rebuild.
    update_stamp(os.path.join(env["WORKDIR"], STAMP_NAME), config.build_hash)

    # Generate manifest and cache a binary package (skip build tools with no $D)
    if os.path.isdir(env["D"]) and os.listdir(env["D"]):
        manifest = generate_manifest(package, env["D"])
        create_binpkg(config, package, env["D"], manifest)

    return True, time.monotonic() - pkg_start


def _commit_to_sysroot(package: Package, d_path: str, sysroot: str):
    """Install built files into sysroot and update world/manifest atomically."""
    if os.path.isdir(d_path) and os.listdir(d_path):
        manifest = generate_manifest(package, d_path)
        conflicts = check_conflicts(manifest, sysroot, exclude_pkg=package.qualified_name)
        if conflicts:
            break_line()
        for path, owner in conflicts:
            print(f"  {yellow('conflict')}: {path} (owned by {owner})")
        shutil.copytree(d_path, sysroot, dirs_exist_ok=True)
        save_installed_manifest(manifest, sysroot)
    if not package.is_build_tool:
        World(sysroot).add(package.qualified_name)


def install_package(
    config: Config, package: Package,
    verbose: bool = False, force: bool = False,
    no_binary: bool = False,
) -> tuple[bool, float]:
    """Install a package into the sysroot. Builds first if needed.

    Returns (success, elapsed_seconds).
    """
    total_start = time.monotonic()
    env = get_build_env(config, package)
    sysroot = env["SYSROOT"]
    reason = build_needed(config, package)

    # Try binary package cache first
    if not (no_binary or force or package.is_build_tool) and reason is None and binpkg_exists(config, package):
        manifest = extract_binpkg(binpkg_path(config, package), sysroot)
        save_installed_manifest(manifest, sysroot)
        World(sysroot).add(package.qualified_name)
        return True, time.monotonic() - total_start

    # Build from source if needed
    if force or reason is not None:
        ok, _ = build_package(config, package, verbose, force=force)
        if not ok:
            return False, time.monotonic() - total_start

    _commit_to_sysroot(package, env["D"], sysroot)
    return True, time.monotonic() - total_start


def _run_stage(stage: str, label: str, makefile: str, env: dict, verbose: bool, log=None) -> tuple[bool, float]:
    """Run a single Make stage, printing a status row. Returns (success, elapsed)."""
    capture = {} if verbose else {"stdout": subprocess.PIPE, "stderr": subprocess.STDOUT, "text": True}
    t0 = time.monotonic()
    result = subprocess.run(
        [env["MAKE"], "-f", makefile, "--no-print-directory", stage],
        env=env, cwd=env["S"], **capture,
    )
    elapsed = time.monotonic() - t0
    ok = result.returncode == 0

    if log is not None:
        log.write(f"### {stage} ({'ok' if ok else 'FAILED'})\n")
        if result.stdout:
            log.write(result.stdout)
        log.flush()
    row = f"  {label:<22}"

    if ok:
        # Quiet mode prints one line per package (in the caller), not per stage.
        if verbose:
            print(f"{row}  {green('✓')}  {dim(fmt_duration(elapsed))}")
    else:
        break_line()
        print(f"{row}  {red('✗')}  FAILED")
        if not verbose and result.stdout:
            print(result.stdout, end="")

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

    os.remove(manifest_path)
    World(sysroot).remove(package.qualified_name)

    return True
