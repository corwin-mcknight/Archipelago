"""Package and configuration validation."""

import os
import re

from graphlib import TopologicalSorter, CycleError

from plume.config import Config
from plume.package import Package

KNOWN_PACKAGE_KEYS = {
    "description", "is_build_tool", "supports_live_sources",
    "live_source_path", "dependencies", "arches", "varies_by",
}

# Axes a package may declare it builds separately for. A package listing an
# axis gets that axis in its qualifier and workdir; packages listing nothing
# resolve at the arch level and are shared.
KNOWN_VARIES_BY = {"board"}


def validate_target(config: Config) -> list[str]:
    """Validate the active target config. Returns errors.

    The board name becomes a path component in workdirs and object trees, so
    it is constrained to the same character set as an arch. A board target
    must also isolate the paths that hold per-board output: boards on one
    arch deliberately share the build tree, so a board inheriting its base's
    sysroot would install its kernel over the other board's and write both
    to the same ISO.
    """
    errors = []
    board = config.get_board()
    if board and not re.match(r"^[a-z0-9_-]+$", board):
        errors.append(f"invalid board name '{board}' (expected lowercase alphanumeric, '_' or '-')")

    arch = config.get_arch()
    # Only a non-default board target needs isolated output paths; the arch
    # config's own board is the one that owns the arch-level sysroot.
    config_name = os.path.basename(config.config_path)[:-5] if config.config_path else ""
    if "^" in config_name:
        expected = f"{arch}^{board}"
        if config_name != expected:
            errors.append(f"config {config_name}.yaml declares arch/board '{expected}'; filename must match")
        for key in ("sysroot", "image_output"):
            base_value = config.base_values.get(key)
            if base_value is not None and config.get(key) == base_value:
                errors.append(
                    f"board target {expected} inherits '{key}' from {arch}; "
                    f"boards share a build tree, so this one must override it"
                )
    return errors


def validate_packages(config: Config, packages: list[Package]) -> list[str]:
    """Validate all packages. Returns errors."""
    errors = validate_target(config)
    by_name = {p.full_name: p for p in packages}

    for pkg in packages:
        if not re.match(r"^[a-z0-9_-]+/[a-z0-9_-]+$", pkg.full_name):
            errors.append(f"{pkg}: invalid package name format (expected category/name)")

        makefile = os.path.join(config.get("repo_path"), "packages", pkg.category, pkg.name, "Makefile")
        if not os.path.exists(makefile):
            errors.append(f"{pkg}: no Makefile at {makefile}")

        # Check dependencies exist and are buildable wherever this package is
        for dep in pkg.dependencies:
            if dep not in by_name:
                errors.append(f"{pkg}: unknown dependency '{dep}'")
            elif pkg.supported and not by_name[dep].supported:
                errors.append(f"{pkg}: dependency '{dep}' is not available for {pkg.arch}")

        if pkg.supports_live_sources and pkg.live_source_path:
            live_path = os.path.join(config.get("source_dir"), pkg.live_source_path)
            if not os.path.isdir(live_path):
                errors.append(f"{pkg}: live_source_path '{pkg.live_source_path}' not found at {live_path}")

    # Check for circular dependencies
    graph = {}
    for pkg in packages:
        deps = set()
        for dep_name in pkg.dependencies:
            if dep_name in by_name:
                deps.add(by_name[dep_name])
        graph[pkg] = deps

    try:
        sorter = TopologicalSorter(graph)
        list(sorter.static_order())
    except CycleError as e:
        errors.append(f"circular dependency detected: {e}")

    return errors


def validate_package_yaml(raw_data: dict) -> list[str]:
    """Check for unknown keys in raw YAML package entries. Returns warnings."""
    warnings = []
    for full_name, pkg_data in raw_data.items():
        if pkg_data is None:
            continue
        unknown = set(pkg_data.keys()) - KNOWN_PACKAGE_KEYS
        if unknown:
            warnings.append(f"{full_name}: unknown keys: {', '.join(sorted(unknown))}")
        unknown_axes = set(pkg_data.get("varies_by") or []) - KNOWN_VARIES_BY
        if unknown_axes:
            warnings.append(f"{full_name}: unknown varies_by axes: {', '.join(sorted(unknown_axes))}")
    return warnings
