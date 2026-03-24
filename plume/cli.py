"""Plume CLI — the Archipelago package manager."""

import argparse
import os
import subprocess
import sys
import time

from plume.config import Config
from plume.output import bold, green, red, yellow, cyan, dim, fmt_duration, fmt_timing_table
from plume.repository import load_packages
from plume.universe import resolve_build_order, filter_packages, expand_with_deps
from plume.builder import build_package, install_package, clean_package, is_built, is_installed
from plume.image import assemble_iso
from plume.world import World
from plume.validate import validate_packages, validate_package_yaml


def _find_config(override=None):
    """Locate config.yaml, checking override, env var, then repo/config.yaml."""
    if override:
        return override
    if "PLUME_CONFIG" in os.environ:
        return os.environ["PLUME_CONFIG"]
    # Walk up from cwd looking for repo/config.yaml
    d = os.getcwd()
    while True:
        candidate = os.path.join(d, "repo", "config.yaml")
        if os.path.exists(candidate):
            return candidate
        parent = os.path.dirname(d)
        if parent == d:
            break
        d = parent
    print(f"{red('plume: error')}: could not find repo/config.yaml", file=sys.stderr)
    sys.exit(1)


def _load(args):
    """Load config and packages, running validation."""
    config_path = _find_config(getattr(args, "config", None))
    config = Config(config_path)
    packages_yml = os.path.join(config.get("repo_path"), "packages.yml")

    import yaml
    with open(packages_yml, "r", encoding="utf-8") as f:
        raw_data = yaml.safe_load(f)

    # Warn about unknown YAML keys
    key_warnings = validate_package_yaml(raw_data or {})
    for w in key_warnings:
        print(f"{yellow('plume: warning')}: {w}", file=sys.stderr)

    packages = load_packages(packages_yml)

    # Run structural validation
    errors, warnings = validate_packages(config, packages)
    for w in warnings:
        print(f"{yellow('plume: warning')}: {w}", file=sys.stderr)
    if errors:
        for e in errors:
            print(f"{red('plume: error')}: {e}", file=sys.stderr)
        sys.exit(1)

    return config, packages


def _print_timing(timings: list[tuple[str, float]], total: float):
    """Print timing summary if there are multiple packages."""
    if len(timings) >= 2:
        print(f"\n{fmt_timing_table(timings, total)}")


def cmd_build(args):
    config, packages = _load(args)
    try:
        requested = filter_packages(packages, args.packages)
        requested_names = {p.full_name for p in requested}
        selected = expand_with_deps(requested, packages)
        ordered = resolve_build_order(selected)
    except ValueError as e:
        print(f"{red('plume: error')}: {e}", file=sys.stderr)
        return 1

    # When building everything, show everything
    show_all = not args.packages

    if args.dry_run:
        shown = [p for p in ordered if show_all or p.full_name in requested_names]
        print(bold("Build order (dry run):"))
        for i, pkg in enumerate(shown, 1):
            print(f"  [{i}/{len(shown)}] {pkg}")
        return 0

    total_start = time.monotonic()
    timings = []
    built_count = 0
    for pkg in ordered:
        is_target = show_all or pkg.full_name in requested_names
        force = args.force if is_target else False
        if not force and is_built(config, pkg):
            continue
        if is_target:
            built_count += 1
            print(bold(cyan(f"\n[{built_count}] {pkg}")))
        ok, elapsed = build_package(config, pkg, verbose=args.verbose if is_target else False, force=force)
        if is_target:
            timings.append((str(pkg), elapsed))
        if not ok:
            print(f"\n{red('Build failed for')} {pkg} after {fmt_duration(time.monotonic() - total_start)}")
            return 1

    total = time.monotonic() - total_start
    if built_count == 0:
        print("Nothing to build.")
    else:
        _print_timing(timings, total)
        print(f"\n{green('Built')} {built_count} package(s) in {fmt_duration(total)}")
    return 0


def cmd_install(args):
    config, packages = _load(args)
    try:
        requested = filter_packages(packages, args.packages)
        requested_names = {p.full_name for p in requested}
        selected = expand_with_deps(requested, packages)
        ordered = resolve_build_order(selected)
    except ValueError as e:
        print(f"{red('plume: error')}: {e}", file=sys.stderr)
        return 1

    show_all = not args.packages

    total_start = time.monotonic()
    timings = []
    installed_count = 0
    for pkg in ordered:
        is_target = show_all or pkg.full_name in requested_names
        force = args.force if is_target else False
        if not force and is_installed(config, pkg):
            continue
        if is_target:
            installed_count += 1
            print(bold(cyan(f"\n[{installed_count}] {pkg}")))
        ok, elapsed = install_package(config, pkg, verbose=args.verbose if is_target else False, force=force)
        if is_target:
            timings.append((str(pkg), elapsed))
        if not ok:
            print(f"\n{red('Install failed for')} {pkg} after {fmt_duration(time.monotonic() - total_start)}")
            return 1

    total = time.monotonic() - total_start
    if installed_count == 0:
        print("Nothing to install.")
    else:
        _print_timing(timings, total)
        print(f"\n{green('Installed')} {installed_count} package(s) in {fmt_duration(total)}")
    return 0


def cmd_rebuild(args):
    """Clean and rebuild specific packages, building deps only if needed."""
    config, packages = _load(args)
    try:
        requested = filter_packages(packages, args.packages)
        requested_names = {p.full_name for p in requested}
        selected = expand_with_deps(requested, packages)
        ordered = resolve_build_order(selected)
    except ValueError as e:
        print(f"{red('plume: error')}: {e}", file=sys.stderr)
        return 1

    total_start = time.monotonic()
    timings = []
    count = len(requested)
    idx = 0
    for pkg in ordered:
        is_target = pkg.full_name in requested_names
        if is_target:
            idx += 1
            print(bold(cyan(f"\n[{idx}/{count}] {pkg}")))
            print(dim(f"  Cleaning {pkg}..."))
            clean_package(config, pkg)
            ok, elapsed = build_package(config, pkg, verbose=args.verbose)
            timings.append((str(pkg), elapsed))
        else:
            # Silently ensure deps are built
            ok, elapsed = build_package(config, pkg)
        if not ok:
            print(f"\n{red('Rebuild failed for')} {pkg} after {fmt_duration(time.monotonic() - total_start)}")
            return 1

    total = time.monotonic() - total_start
    _print_timing(timings, total)
    print(f"\n{green('Rebuilt')} {count} package(s) in {fmt_duration(total)}")
    return 0


def cmd_image(args):
    config, _ = _load(args)
    verbose = getattr(args, "verbose", False)
    print(bold("Assembling ISO image"))
    if not assemble_iso(config, verbose=verbose):
        return 1
    print(f"{green('ISO written to')} {config.get('iso_output')}")
    return 0


def cmd_test(args):
    config, packages = _load(args)

    # Install all packages (builds if needed, updates world)
    ordered = resolve_build_order(packages)
    total_start = time.monotonic()
    timings = []
    for i, pkg in enumerate(ordered, 1):
        print(bold(cyan(f"\n[{i}/{len(ordered)}] {pkg}")))
        ok, elapsed = install_package(config, pkg, verbose=args.verbose, force=args.force)
        timings.append((str(pkg), elapsed))
        if not ok:
            print(f"\n{red('Build failed')} after {fmt_duration(time.monotonic() - total_start)}", file=sys.stderr)
            return 1

    total = time.monotonic() - total_start
    _print_timing(timings, total)

    # Assemble ISO
    print(bold("\nAssembling ISO image"))
    if not assemble_iso(config, verbose=args.verbose):
        return 1

    # Run test harness
    print("\n\nRunning tests...\n")
    harness_args = [sys.executable, "tools/test-harness.py"]
    if args.verbose:
        harness_args.append("--verbose")
    harness_args.extend(args.tests)
    result = subprocess.run(harness_args, cwd=config.project_root)
    return result.returncode


def cmd_status(args):
    config, packages = _load(args)
    world = World(config.get("sysroot"))
    installed = set(world.read())

    name_width = max((len(p.full_name) for p in packages), default=10)
    header = f"  {'Package':<{name_width}}  {'Built':<7}  {'Installed'}"
    print(bold(header))
    print(f"  {'─' * (name_width + 22)}")

    for pkg in packages:
        built = is_built(config, pkg)
        in_world = pkg.full_name in installed

        built_mark = green("✓") if built else dim("–")
        inst_mark = green("✓") if in_world else (dim("–") if not pkg.is_build_tool else dim("n/a"))

        tags = ""
        if pkg.is_build_tool:
            tags += dim("  [build-tool]")
        if pkg.supports_live_sources:
            tags += dim("  [live]")

        print(f"  {pkg.full_name:<{name_width}}  {built_mark:<7}  {inst_mark}{tags}")

    return 0


def cmd_validate(args):
    config, packages = _load(args)
    # _load already runs validation and exits on errors; if we reach here, all good
    print(f"{green('✓')} All {len(packages)} package(s) valid.")
    return 0


def cmd_world(args):
    config, _ = _load(args)
    world = World(config.get("sysroot"))
    entries = world.read()

    if not entries:
        print("No packages installed.")
        return 0

    for entry in entries:
        print(f"  {entry}")
    return 0


def cmd_clean(args):
    config, _ = _load(args)
    import shutil
    for d in ["obj", "sysroot", "tmp"]:
        p = os.path.join(config.get("build_dir"), d)
        if os.path.exists(p):
            shutil.rmtree(p)
    iso = config.get("iso_output")
    if os.path.exists(iso):
        os.remove(iso)
    print("Cleaned build artifacts.")
    return 0


def cmd_list(args):
    _, packages = _load(args)

    if args.tree:
        for pkg in packages:
            tags = ""
            if pkg.is_build_tool:
                tags += dim("  [build-tool]")
            if pkg.supports_live_sources:
                tags += dim("  [live]")
            print(f"{bold(pkg.full_name)}{tags}")
            for i, dep_name in enumerate(pkg.dependencies):
                connector = "└─" if i == len(pkg.dependencies) - 1 else "├─"
                print(f"  {connector} {dep_name}")
        return 0

    for pkg in packages:
        tool = " [build-tool]" if pkg.is_build_tool else ""
        live = " [live]" if pkg.supports_live_sources else ""
        print(f"  {pkg.full_name:30s} {pkg.description}{tool}{live}")
    return 0


def cmd_clangd(args):
    config, packages = _load(args)

    # Force-rebuild kernel to regenerate -MJ fragments
    kernel_pkgs = [p for p in packages if p.name == "kernel"]
    if not kernel_pkgs:
        print(f"{red('plume: error')}: no kernel package found", file=sys.stderr)
        return 1

    # Force rebuild with -B
    from plume.env import get_build_env
    env = get_build_env(config, kernel_pkgs[0])
    os.makedirs(os.path.join(config.get("build_dir"), "obj", "sys", "kernel"), exist_ok=True)
    result = subprocess.run(
        [env["MAKE"], "-B", "-j", env["MAKE_JOBS"], f"BUILD_DIR={config.get('build_dir')}"],
        cwd=env["LIVE_SOURCES"],
        env=env,
    )
    if result.returncode != 0:
        print(f"{red('plume: error')}: kernel rebuild failed", file=sys.stderr)
        return 1

    # Merge compile commands
    result = subprocess.run(
        [sys.executable, "tools/merge-compile-commands.py",
         config.get("build_dir"),
         os.path.join(config.get("build_dir"), "compile_commands.json")],
        cwd=config.project_root,
    )
    return result.returncode


def main(argv=None):
    parser = argparse.ArgumentParser(prog="plume", description="Plume — the Archipelago package manager")
    sub = parser.add_subparsers(dest="command")

    build_p = sub.add_parser("build", help="Build packages (does not install to sysroot)")
    build_p.add_argument("packages", nargs="*", help="Packages to build (default: all, supports @set)")
    build_p.add_argument("--config", default=None)
    build_p.add_argument("--verbose", "-v", action="store_true", help="Stream make output instead of capturing")
    build_p.add_argument("--dry-run", "-n", action="store_true", help="Print build order without building")
    build_p.add_argument("--force", "-f", action="store_true", help="Force rebuild even if already built")

    install_p = sub.add_parser("install", help="Install packages into sysroot (builds if needed)")
    install_p.add_argument("packages", nargs="*", help="Packages to install (default: all, supports @set)")
    install_p.add_argument("--config", default=None)
    install_p.add_argument("--verbose", "-v", action="store_true", help="Stream make output instead of capturing")
    install_p.add_argument("--force", "-f", action="store_true", help="Force rebuild even if already built")

    rebuild_p = sub.add_parser("rebuild", help="Clean and rebuild specific packages")
    rebuild_p.add_argument("packages", nargs="*", help="Packages to rebuild (default: all, supports @set)")
    rebuild_p.add_argument("--config", default=None)
    rebuild_p.add_argument("--verbose", "-v", action="store_true", help="Stream make output instead of capturing")

    image_p = sub.add_parser("image", help="Assemble ISO image")
    image_p.add_argument("--config", default=None)
    image_p.add_argument("--verbose", "-v", action="store_true", help="Show xorriso and limine output")

    test_p = sub.add_parser("test", help="Build and run tests")
    test_p.add_argument("tests", nargs="*")
    test_p.add_argument("--verbose", action="store_true")
    test_p.add_argument("--force", "-f", action="store_true", help="Force rebuild even if already built")
    test_p.add_argument("--config", default=None)

    status_p = sub.add_parser("status", help="Show build and install state of all packages")
    status_p.add_argument("--config", default=None)

    validate_p = sub.add_parser("validate", help="Validate packages.yml and package Makefiles")
    validate_p.add_argument("--config", default=None)

    clean_p = sub.add_parser("clean", help="Remove build artifacts")
    clean_p.add_argument("--config", default=None)

    list_p = sub.add_parser("list", help="List packages")
    list_p.add_argument("--config", default=None)
    list_p.add_argument("--tree", action="store_true", help="Show dependency tree")

    world_p = sub.add_parser("world", help="Show packages installed in the sysroot")
    world_p.add_argument("--config", default=None)

    clangd_p = sub.add_parser("clangd", help="Regenerate compile_commands.json")
    clangd_p.add_argument("--config", default=None)

    args = parser.parse_args(argv)
    if not args.command:
        parser.print_help()
        return 1

    commands = {
        "build": cmd_build,
        "install": cmd_install,
        "rebuild": cmd_rebuild,
        "image": cmd_image,
        "test": cmd_test,
        "status": cmd_status,
        "validate": cmd_validate,
        "clean": cmd_clean,
        "list": cmd_list,
        "world": cmd_world,
        "clangd": cmd_clangd,
    }
    return commands[args.command](args)
