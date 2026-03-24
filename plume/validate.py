"""Package and configuration validation."""

import os
import re

from graphlib import TopologicalSorter, CycleError

from plume.config import Config
from plume.package import Package

KNOWN_PACKAGE_KEYS = {
    "description", "is_build_tool", "supports_live_sources",
    "live_source_path", "dependencies", "source",
}


def validate_packages(config: Config, packages: list[Package]) -> tuple[list[str], list[str]]:
    """Validate all packages. Returns (errors, warnings)."""
    errors = []
    warnings = []
    by_name = {p.full_name: p for p in packages}

    for pkg in packages:
        # Check name format
        if not re.match(r"^[a-z0-9_-]+/[a-z0-9_-]+-[a-zA-Z0-9._]+$", pkg.full_name):
            errors.append(f"{pkg}: invalid package name format (expected category/name-version)")

        # Check Makefile exists
        makefile = os.path.join(config.get("repo_path"), "packages", pkg.category, pkg.name, "Makefile")
        if not os.path.exists(makefile):
            errors.append(f"{pkg}: no Makefile at {makefile}")

        # Check dependencies exist
        for dep in pkg.dependencies:
            if dep not in by_name:
                errors.append(f"{pkg}: unknown dependency '{dep}'")

        # Check live source path exists
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

    return errors, warnings


def validate_package_yaml(raw_data: dict) -> list[str]:
    """Check for unknown keys in raw YAML package entries. Returns warnings."""
    warnings = []
    for full_name, pkg_data in raw_data.items():
        if pkg_data is None:
            continue
        unknown = set(pkg_data.keys()) - KNOWN_PACKAGE_KEYS
        if unknown:
            warnings.append(f"{full_name}: unknown keys: {', '.join(sorted(unknown))}")
    return warnings
