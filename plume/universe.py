"""Dependency resolution via topological sort."""

import os
from collections import deque

from graphlib import TopologicalSorter

from plume.package import Package


def reverse_deps(targets: list[Package], all_packages: list[Package]) -> list[Package]:
    """Return targets plus all packages that transitively depend on them.

    Builds a reverse adjacency map and BFS-expands from targets.
    """
    # Build reverse map: dep_name -> set of packages that depend on it
    rev: dict[str, set[str]] = {}
    by_name = {p.full_name: p for p in all_packages}
    for pkg in all_packages:
        for dep_name in pkg.dependencies:
            rev.setdefault(dep_name, set()).add(pkg.full_name)

    # BFS from targets through reverse edges
    visited: set[str] = set()
    queue = deque(t.full_name for t in targets)
    while queue:
        name = queue.popleft()
        if name in visited:
            continue
        visited.add(name)
        for rdep in rev.get(name, ()):
            if rdep not in visited:
                queue.append(rdep)

    return [by_name[n] for n in visited if n in by_name]


def expand_with_deps(selected: list[Package], all_packages: list[Package]) -> list[Package]:
    """Expand a package list to include all transitive dependencies.

    Packages already in selected are kept; missing deps are pulled from all_packages.
    """
    by_name = {p.full_name: p for p in all_packages}
    result = {}
    queue = list(selected)
    while queue:
        pkg = queue.pop()
        if pkg.full_name in result:
            continue
        result[pkg.full_name] = pkg
        for dep_name in pkg.dependencies:
            if dep_name not in result and dep_name in by_name:
                queue.append(by_name[dep_name])
    return list(result.values())


def _build_dep_graph(packages: list[Package]) -> dict[Package, set[Package]]:
    """Build a dependency graph suitable for TopologicalSorter."""
    by_name = {p.full_name: p for p in packages}
    graph = {}
    for pkg in packages:
        deps = set()
        for dep_name in pkg.dependencies:
            if dep_name not in by_name:
                raise ValueError(f"Unknown dependency '{dep_name}' required by {pkg}")
            deps.add(by_name[dep_name])
        graph[pkg] = deps
    return graph


def resolve_build_order(packages: list[Package]) -> list[Package]:
    """Return packages in dependency order (dependencies first)."""
    return list(TopologicalSorter(_build_dep_graph(packages)).static_order())


def create_build_sorter(packages: list[Package]) -> TopologicalSorter:
    """Return a prepared TopologicalSorter for level-based parallel execution."""
    sorter = TopologicalSorter(_build_dep_graph(packages))
    sorter.prepare()
    return sorter


def _package_lookups(all_packages: list[Package]) -> tuple[dict[str, Package], dict[str, Package]]:
    """Build by-full-name and by-short-name lookup dicts."""
    by_name = {p.full_name: p for p in all_packages}
    by_short = {f"{p.category}/{p.name}": p for p in all_packages}
    return by_name, by_short


def _lookup_package(name: str, by_name: dict, by_short: dict, context: str = "") -> Package:
    """Resolve a package name, raising ValueError if not found."""
    if name in by_name:
        return by_name[name]
    if name in by_short:
        return by_short[name]
    msg = f"Unknown package '{name}'" + (f" in set @{context}" if context else "")
    raise ValueError(msg)


def filter_packages(all_packages: list[Package], requested: list[str]) -> list[Package]:
    """Filter packages by name or @set reference. If requested is empty, return all.

    Packages gated to other architectures are dropped from sets and the
    all-packages default; naming one explicitly is an error.
    """
    if not requested:
        return [p for p in all_packages if p.supported]

    by_name, by_short = _package_lookups(all_packages)
    result = []
    for name in requested:
        if name.startswith("@"):
            result.extend(p for p in load_set(name[1:], all_packages, _lookups=(by_name, by_short)) if p.supported)
        else:
            pkg = _lookup_package(name, by_name, by_short)
            if not pkg.supported:
                raise ValueError(f"{pkg.full_name} is not available for {pkg.arch} (arches: {', '.join(pkg.arches)})")
            result.append(pkg)

    # Deduplicate preserving order
    seen = set()
    return [p for p in result if not (p.full_name in seen or seen.add(p.full_name))]


def load_set(set_name: str, all_packages: list[Package],
             repo_path: str | None = None, _lookups=None) -> list[Package]:
    """Load a package set from repo/sets/<set_name>."""
    if repo_path is None:
        d = os.getcwd()
        while True:
            candidate = os.path.join(d, "repo", "sets", set_name)
            if os.path.exists(candidate):
                set_path = candidate
                break
            parent = os.path.dirname(d)
            if parent == d:
                raise ValueError(f"Set file not found: @{set_name}")
            d = parent
    else:
        set_path = os.path.join(repo_path, "sets", set_name)

    if not os.path.exists(set_path):
        raise ValueError(f"Set file not found: {set_path}")

    with open(set_path, "r", encoding="utf-8") as f:
        entries = [line.strip() for line in f if line.strip()]

    if _lookups:
        by_name, by_short = _lookups
    else:
        by_name, by_short = _package_lookups(all_packages)

    return [_lookup_package(e, by_name, by_short, context=set_name) for e in entries]
