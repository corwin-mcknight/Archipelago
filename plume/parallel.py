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
            futures = {}
            for pkg in sorter.get_ready():
                force = pkg.full_name in force_set
                if (not force and skip_built and is_built(config, pkg)) or failed:
                    sorter.done(pkg)
                    continue
                futures[executor.submit(build_package, config, pkg, verbose=False, force=force)] = pkg

            for fut in as_completed(futures):
                pkg = futures[fut]
                ok, elapsed = fut.result()
                timings.append((str(pkg), elapsed))
                sorter.done(pkg)
                if ok:
                    idx += 1
                    _print_sync(f"{bold(cyan(f'[{idx}/{total}]'))} {green('Built')} {pkg}  {dim(fmt_duration(elapsed))}")
                else:
                    _print_sync(f"{red('Build failed for')} {pkg}")
                    failed = True

    return (not failed, timings)
