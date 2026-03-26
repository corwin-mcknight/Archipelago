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


def resolve_build_order(packages: list[Package]) -> list[Package]:
    """Return packages in dependency order (dependencies first)."""
    by_name = {p.full_name: p for p in packages}

    graph = {}
    for pkg in packages:
        deps = set()
        for dep_name in pkg.dependencies:
            if dep_name not in by_name:
                raise ValueError(f"Unknown dependency '{dep_name}' required by {pkg}")
            deps.add(by_name[dep_name])
        graph[pkg] = deps

    sorter = TopologicalSorter(graph)
    return list(sorter.static_order())


def create_build_sorter(packages: list[Package]) -> TopologicalSorter:
    """Return a prepared TopologicalSorter for level-based parallel execution.

    The caller drives the sorter with get_ready() / done().
    """
    by_name = {p.full_name: p for p in packages}

    graph = {}
    for pkg in packages:
        deps = set()
        for dep_name in pkg.dependencies:
            if dep_name not in by_name:
                raise ValueError(f"Unknown dependency '{dep_name}' required by {pkg}")
            deps.add(by_name[dep_name])
        graph[pkg] = deps

    sorter = TopologicalSorter(graph)
    sorter.prepare()
    return sorter


def filter_packages(all_packages: list[Package], requested: list[str]) -> list[Package]:
    """Filter packages by name or @set reference. If requested is empty, return all.

    A name starting with '@' is treated as a set reference (e.g. '@system')
    and is expanded by reading repo/sets/<name>.
    """
    if not requested:
        return list(all_packages)

    by_name = {p.full_name: p for p in all_packages}
    # Also allow matching by just "category/name" without version
    by_short = {f"{p.category}/{p.name}": p for p in all_packages}

    result = []
    for name in requested:
        if name.startswith("@"):
            set_entries = load_set(name[1:], all_packages)
            result.extend(set_entries)
        elif name in by_name:
            result.append(by_name[name])
        elif name in by_short:
            result.append(by_short[name])
        else:
            raise ValueError(f"Unknown package: {name}")

    # Deduplicate while preserving order
    seen = set()
    deduped = []
    for pkg in result:
        if pkg.full_name not in seen:
            seen.add(pkg.full_name)
            deduped.append(pkg)
    return deduped


def load_set(set_name: str, all_packages: list[Package], repo_path: str | None = None) -> list[Package]:
    """Load a package set from repo/sets/<set_name>.

    The set file is a newline-separated list of package full names.
    If repo_path is None, it is inferred from the first package's FILESDIR
    or falls back to searching upward for repo/sets/.
    """
    if repo_path is None:
        # Walk up from cwd to find repo/sets/
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

    by_name = {p.full_name: p for p in all_packages}
    by_short = {f"{p.category}/{p.name}": p for p in all_packages}

    packages = []
    for entry in entries:
        if entry in by_name:
            packages.append(by_name[entry])
        elif entry in by_short:
            packages.append(by_short[entry])
        else:
            raise ValueError(f"Unknown package '{entry}' in set @{set_name}")
    return packages
