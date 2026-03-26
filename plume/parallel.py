"""Parallel package build executor."""

import threading
from concurrent.futures import ThreadPoolExecutor, as_completed

from plume.builder import build_package, is_built
from plume.config import Config
from plume.output import bold, cyan, green, red, dim, fmt_duration
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
    verbose: bool = False,
    force_set: set[str] | None = None,
    skip_built: bool = True,
) -> tuple[bool, list[tuple[str, float]]]:
    """Build packages in parallel respecting dependency order.

    Uses TopologicalSorter's iterative API to dispatch independent packages
    concurrently via a thread pool.  Verbose output is always captured in
    parallel mode and printed per-package on completion to avoid interleaving.

    Returns (overall_success, list of (package_name, elapsed) for built pkgs).
    """
    if force_set is None:
        force_set = set()

    sorter = create_build_sorter(packages)
    timings: list[tuple[str, float]] = []
    failed = False
    idx = 0
    total = sum(1 for p in packages if p.full_name in force_set or not (skip_built and is_built(config, p)))

    with ThreadPoolExecutor(max_workers=max_workers) as executor:
        while sorter.is_active():
            ready = sorter.get_ready()

            # Separate skippable from buildable
            futures = {}
            for pkg in ready:
                force = pkg.full_name in force_set
                if not force and skip_built and is_built(config, pkg):
                    sorter.done(pkg)
                    continue
                if failed:
                    # Don't submit new work after a failure
                    sorter.done(pkg)
                    continue
                fut = executor.submit(
                    build_package, config, pkg,
                    verbose=False,  # Always capture in parallel mode
                    force=force,
                )
                futures[fut] = pkg

            for fut in as_completed(futures):
                pkg = futures[fut]
                ok, elapsed = fut.result()
                if ok:
                    idx += 1
                    _print_sync(f"{bold(cyan(f'[{idx}/{total}]'))} {green('Built')} {pkg}  {dim(fmt_duration(elapsed))}")
                    timings.append((str(pkg), elapsed))
                    sorter.done(pkg)
                else:
                    _print_sync(f"{red('Build failed for')} {pkg}")
                    failed = True
                    timings.append((str(pkg), elapsed))
                    sorter.done(pkg)

    return (not failed, timings)
