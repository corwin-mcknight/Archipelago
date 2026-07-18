"""Parallel package build executor."""

import threading
from concurrent.futures import FIRST_COMPLETED, ThreadPoolExecutor, wait

from plume.builder import build_package, build_needed
from plume.config import Config
from plume.output import bold, cyan, green, red, dim, fmt_duration, fmt_reason
from plume.package import Package
from plume.universe import create_build_sorter


_print_lock = threading.Lock()


def _print_sync(*args, **kwargs):
    with _print_lock:
        print(*args, **kwargs)


def parallel_build(
    config: Config,
    packages: list[Package],
    *,
    max_workers: int = 1,
    force_set: set[str] | None = None,
    keep_going: bool = False,
) -> tuple[bool, list[tuple[str, float]]]:
    """Build packages in parallel respecting dependency order.

    Returns (overall_success, list of (package_name, elapsed) for built pkgs).
    """
    if force_set is None:
        force_set = set()

    sorter = create_build_sorter(packages)
    timings: list[tuple[str, float]] = []
    failed = False
    dead: set[str] = set()  # failed packages and their transitive dependents
    idx = 0
    # Upfront count for the [i/total] labels only; each package is re-checked
    # just before submission, since sources can change while others build.
    total = sum(
        1 for p in packages
        if p.full_name in force_set or build_needed(config, p) is not None
    )

    with ThreadPoolExecutor(max_workers=max_workers) as executor:
        pending = {}

        def submit_ready():
            # Skipped packages unlock their successors immediately, so keep draining.
            while ready := sorter.get_ready():
                for pkg in ready:
                    force = pkg.full_name in force_set
                    # Capture the staleness reason now; the build refreshes the stamp.
                    reason = build_needed(config, pkg)
                    if any(d in dead for d in pkg.dependencies):
                        # Topological release order makes this transitive.
                        dead.add(pkg.full_name)
                        _print_sync(dim(f"  skipped {pkg} (dependency failed)"))
                        sorter.done(pkg)
                    elif (failed and not keep_going) or (not force and reason is None):
                        sorter.done(pkg)
                    else:
                        fut = executor.submit(build_package, config, pkg, verbose=False, force=force)
                        pending[fut] = (pkg, reason)

        submit_ready()
        while pending:
            completed, _ = wait(pending, return_when=FIRST_COMPLETED)
            for fut in completed:
                pkg, reason = pending.pop(fut)
                ok, elapsed = fut.result()
                timings.append((str(pkg), elapsed))
                sorter.done(pkg)
                if ok:
                    idx += 1
                    total = max(total, idx)
                    _print_sync(f"{bold(cyan(f'[{idx}/{total}]'))} {green('Built')} {pkg}  {dim(fmt_duration(elapsed))}{fmt_reason(reason)}")
                else:
                    _print_sync(f"{red('Build failed for')} {pkg}")
                    failed = True
                    dead.add(pkg.full_name)
            submit_ready()

    return (not failed, timings)
