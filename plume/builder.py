"""Build executor: runs package Makefile stages and composes the sysroot.

The sysroot is derived, never mutated across runs: whenever any system
package (re)builds or the composed set changes, it is rebuilt from scratch
as the union of the current packages' staging trees ($D), copied in
dependency order so a package always compiles against its dependencies'
installed files. There is no installed-package database -- what is in the
sysroot is exactly what the repository defines, and two packages claiming
the same path is an error caught at compose time.
"""

import hashlib
import os
import shutil
import subprocess
import sys
import time
from collections import deque
from concurrent.futures import FIRST_COMPLETED, ThreadPoolExecutor, wait

from plume.config import Config
from plume.output import bold, green, red, cyan, dim, fmt_duration, fmt_reason, open_line, close_line, break_line
from plume.package import Package
from plume.env import get_build_env
from plume.stamp import STAMP_NAME, is_stale, update as update_stamp
from plume.universe import create_build_sorter


STAGES = [
    ("pkg_get_source", "Get Source Files"),
    ("pkg_configure",  "Configure"),
    ("pkg_build",      "Build"),
    ("pkg_install",    "Install"),
]


def _source_paths(config: Config, package: Package) -> list[str]:
    """Input trees whose content decides staleness: the package's own files,
    plus the live source tree for live-source packages."""
    env = get_build_env(config, package)
    paths = [env["FILESDIR"]]
    if package.supports_live_sources and package.live_source_path:
        paths.append(env["LIVE_SOURCES"])
    return paths


def _stamp_path(config: Config, package: Package) -> str:
    return os.path.join(get_build_env(config, package)["WORKDIR"], STAMP_NAME)


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

    return is_stale(_stamp_path(config, package), config.build_hash, _source_paths(config, package))


def build_log_path(config: Config, package: Package) -> str:
    """Path of the persisted build log for a package."""
    return os.path.join(config.get("build_dir"), "logs", package.category, f"{package.name}.log")


def build_package(config: Config, package: Package, verbose: bool = False, force: bool = False) -> tuple[bool, float]:
    """Build a single package by running its Makefile stages.

    This only builds into staging ($D) -- composing into the sysroot is the
    orchestrator's job. Returns (success, elapsed_seconds).
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

    # Record the inputs so a config or source change triggers a rebuild.
    update_stamp(_stamp_path(config, package), config.build_hash, _source_paths(config, package))

    return True, time.monotonic() - pkg_start


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


def sysroot_stamp_path(config: Config) -> str:
    """Sibling stamp recording what the sysroot was last composed from."""
    return config.get("sysroot").rstrip(os.sep) + ".stamp"


def sysroot_identity(config: Config, compose_set: list[Package]) -> str:
    """Hash of the composed set and each member's build stamp.

    Deterministic function of (which packages, which builds), so it changes
    exactly when the sysroot's derivation does.
    """
    h = hashlib.sha256()
    for pkg in sorted(compose_set, key=lambda p: p.qualified_name):
        h.update(pkg.qualified_name.encode())
        try:
            with open(_stamp_path(config, pkg), "rb") as f:
                h.update(f.read())
        except OSError:
            h.update(b"unbuilt")
    return h.hexdigest()[:16]


def sysroot_needs_compose(config: Config, compose_set: list[Package]) -> str | None:
    """Return why the sysroot must be recomposed, or None if it is current."""
    if not os.path.isdir(config.get("sysroot")):
        return "sysroot missing"
    try:
        with open(sysroot_stamp_path(config), "r", encoding="utf-8") as f:
            recorded = f.read().strip()
    except OSError:
        return "never composed"
    if recorded != sysroot_identity(config, compose_set):
        return "package set or builds changed"
    return None


def _commit(label: str, d: str, sysroot: str, owners: dict[str, str]) -> bool:
    """Copy a staging tree into the sysroot being composed.

    Two packages claiming one path is an error, not a warning: the sysroot is
    the union of the staging trees, so overlap means the union is ambiguous.
    """
    if not (os.path.isdir(d) and os.listdir(d)):
        return True
    ok = True
    for root, _dirs, files in os.walk(d):
        for name in files:
            rel = os.path.relpath(os.path.join(root, name), d)
            if rel in owners:
                break_line()
                print(f"  {red('conflict')}: {rel} installed by both {owners[rel]} and {label}", file=sys.stderr)
                ok = False
            owners[rel] = label
    if not ok:
        return False
    shutil.copytree(d, sysroot, dirs_exist_ok=True)
    return True


def orchestrate(config: Config, ordered: list[Package], compose_set: list[Package], *,
                jobs: int = 1, force_set: set[str] = frozenset(), verbose: bool = False,
                keep_going: bool = False) -> int:
    """Build every stale package in *ordered* and compose the sysroot.

    *ordered* is the topologically sorted build graph; *compose_set* is the
    subset whose staging trees form the sysroot (empty for tool-only runs).
    When composing, the sysroot is rebuilt from scratch: removed up front,
    then each compose-set package is copied in as soon as it is current, so
    later packages compile against their dependencies' installed files.
    """
    total_start = time.monotonic()
    compose_names = {p.full_name for p in compose_set}
    recompose = None
    if compose_set:
        recompose = sysroot_needs_compose(config, compose_set)
        if recompose is None and any(
            p.full_name in force_set or build_needed(config, p) is not None for p in compose_set
        ):
            recompose = "builds pending"

    sysroot = config.get("sysroot")
    if recompose:
        if os.path.isdir(sysroot):
            shutil.rmtree(sysroot)
        os.makedirs(sysroot)

    owners: dict[str, str] = {}
    dead: set[str] = set()  # failed packages and their transitive dependents
    failures: list[str] = []
    built = 0
    # Upfront count for the [i/total] labels only; the input walk is cached
    # per invocation, so this costs one walk, not two.
    total = sum(1 for p in ordered if p.full_name in force_set or build_needed(config, p) is not None)

    def commit(pkg) -> bool:
        if not recompose or pkg.full_name not in compose_names:
            return True
        if _commit(str(pkg), get_build_env(config, pkg)["D"], sysroot, owners):
            return True
        dead.add(pkg.full_name)
        failures.append(pkg.full_name)
        return False

    def skip_dead(pkg) -> bool:
        if any(d in dead for d in pkg.dependencies):
            dead.add(pkg.full_name)
            print(dim(f"  skipped {pkg} (dependency failed)"))
            return True
        return False

    if jobs == 1:
        for pkg in ordered:
            if skip_dead(pkg):
                continue
            if failures and not keep_going:
                break
            force = pkg.full_name in force_set
            reason = build_needed(config, pkg)
            if not force and reason is None:
                commit(pkg)
                continue
            built += 1
            total = max(total, built)
            label = bold(cyan(f"[{built}/{total}] {pkg}"))
            print(label, flush=True) if verbose else open_line(label)
            ok, elapsed = build_package(config, pkg, verbose=verbose, force=force)
            if ok and commit(pkg):
                text = f"{green('✓')}  {dim(fmt_duration(elapsed))}{fmt_reason(reason)}"
                print(f"  {text}") if verbose else close_line(text)
            elif pkg.full_name not in dead:
                dead.add(pkg.full_name)
                failures.append(pkg.full_name)
    else:
        sorter = create_build_sorter(ordered)
        ready: deque = deque()
        pending: dict = {}

        def pull():
            ready.extend(sorter.get_ready())

        with ThreadPoolExecutor(max_workers=jobs) as executor:
            pull()
            while ready or pending:
                while ready and len(pending) < jobs:
                    pkg = ready.popleft()
                    force = pkg.full_name in force_set
                    reason = build_needed(config, pkg)
                    if skip_dead(pkg) or (failures and not keep_going):
                        sorter.done(pkg)
                    elif not force and reason is None:
                        # Commit before done() releases dependents: they compile
                        # against the sysroot. All sysroot writes stay on this
                        # thread, so -j needs no lock around them.
                        commit(pkg)
                        sorter.done(pkg)
                    else:
                        pending[executor.submit(build_package, config, pkg, verbose=False, force=force)] = (pkg, reason)
                    pull()
                if not pending:
                    continue
                completed, _ = wait(pending, return_when=FIRST_COMPLETED)
                for fut in completed:
                    pkg, reason = pending.pop(fut)
                    ok, elapsed = fut.result()
                    if ok and commit(pkg):
                        built += 1
                        total = max(total, built)
                        print(f"{bold(cyan(f'[{built}/{total}]'))} {green('Built')} {pkg}  "
                              f"{dim(fmt_duration(elapsed))}{fmt_reason(reason)}")
                    elif pkg.full_name not in dead:
                        print(f"{red('Build failed for')} {pkg}")
                        dead.add(pkg.full_name)
                        failures.append(pkg.full_name)
                    sorter.done(pkg)
                    pull()

    elapsed = fmt_duration(time.monotonic() - total_start)
    if failures:
        print(f"\n{red('Failed to build')} {len(failures)} package(s): {', '.join(failures)}")
        return 1
    if recompose:
        with open(sysroot_stamp_path(config), "w", encoding="utf-8") as f:
            f.write(sysroot_identity(config, compose_set) + "\n")
    if built == 0:
        print("Nothing to build; sysroot current." if compose_set and not recompose else "Nothing to build.")
    else:
        print(f"\n{green('Built')} {built} package(s) in {elapsed}")
    if recompose:
        print(f"{green('Sysroot composed')} from {len(compose_set)} package(s) ({dim(recompose)})")
    return 0
