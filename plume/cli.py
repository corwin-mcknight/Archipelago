"""Plume CLI -- the Archipelago package manager."""

import argparse
import os
import shutil
import subprocess
import sys
import time

from plume.config import Config
from plume.output import bold, green, red, yellow, cyan, dim, fmt_duration, fmt_timing_table
from plume.repository import load_packages
from plume.universe import resolve_build_order, filter_packages, expand_with_deps, reverse_deps
from plume.builder import build_package, install_package, uninstall_package, clean_package, is_built, is_installed
from plume.image import assemble_iso
from plume.world import World
from plume.validate import validate_packages, validate_package_yaml


def _find_config(override=None):
    """Locate config.yaml, checking override, env var, then repo/config.yaml."""
    if override:
        return override
    if "PLUME_CONFIG" in os.environ:
        return os.environ["PLUME_CONFIG"]
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

    for w in validate_package_yaml(raw_data or {}):
        print(f"{yellow('plume: warning')}: {w}", file=sys.stderr)

    packages = load_packages(packages_yml, arch=config.get_arch())

    errors, warnings = validate_packages(config, packages)
    for w in warnings:
        print(f"{yellow('plume: warning')}: {w}", file=sys.stderr)
    if errors:
        for e in errors:
            print(f"{red('plume: error')}: {e}", file=sys.stderr)
        sys.exit(1)

    return config, packages


def _resolve(packages, requested):
    """Filter, expand deps, and resolve build order. Returns (ordered, requested_names)."""
    selected = filter_packages(packages, requested)
    names = {p.full_name for p in selected}
    expanded = expand_with_deps(selected, packages)
    return resolve_build_order(expanded), names


def _print_timing(timings, total):
    if len(timings) >= 2:
        print(f"\n{fmt_timing_table(timings, total)}")


def _sequential_run(ordered, requested_names, show_all, config, args, action, action_label, **action_kwargs):
    """Run action on each package sequentially, tracking timing. Returns (return_code, timings, total)."""
    total_start = time.monotonic()
    timings = []
    count = 0
    for pkg in ordered:
        is_target = show_all or pkg.full_name in requested_names
        force = getattr(args, "force", False) if is_target else False
        skip_check = is_built if action == build_package else is_installed
        if not force and skip_check(config, pkg):
            continue
        if is_target:
            count += 1
            print(bold(cyan(f"\n[{count}] {pkg}")))
        ok, elapsed = action(
            config, pkg,
            verbose=args.verbose if is_target else False,
            force=force, **action_kwargs,
        )
        if is_target:
            timings.append((str(pkg), elapsed))
        if not ok:
            total = time.monotonic() - total_start
            print(f"\n{red(f'{action_label} failed for')} {pkg} after {fmt_duration(total)}")
            return 1, timings, total

    total = time.monotonic() - total_start
    if count == 0:
        print(f"Nothing to {action_label.lower()}.")
    else:
        _print_timing(timings, total)
        print(f"\n{green(action_label)} {count} package(s) in {fmt_duration(total)}")
    return 0, timings, total


def cmd_build(args):
    config, packages = _load(args)
    try:
        ordered, requested_names = _resolve(packages, args.packages)
    except ValueError as e:
        print(f"{red('plume: error')}: {e}", file=sys.stderr)
        return 1

    show_all = not args.packages

    if args.dry_run:
        shown = [p for p in ordered if show_all or p.full_name in requested_names]
        print(bold("Build order (dry run):"))
        for i, pkg in enumerate(shown, 1):
            print(f"  [{i}/{len(shown)}] {pkg}")
        return 0

    jobs = getattr(args, "jobs", 1)
    if jobs > 1:
        from plume.parallel import parallel_build
        force_set = requested_names if args.force else set()
        total_start = time.monotonic()
        ok, timings = parallel_build(config, ordered, max_workers=jobs, verbose=args.verbose, force_set=force_set)
        total = time.monotonic() - total_start
        if not timings:
            print("Nothing to build.")
        else:
            _print_timing(timings, total)
            if ok:
                print(f"\n{green('Built')} {len(timings)} package(s) in {fmt_duration(total)}")
        return 0 if ok else 1

    rc, _, _ = _sequential_run(ordered, requested_names, show_all, config, args, build_package, "Built")
    return rc


def cmd_install(args):
    config, packages = _load(args)
    try:
        ordered, requested_names = _resolve(packages, args.packages)
    except ValueError as e:
        print(f"{red('plume: error')}: {e}", file=sys.stderr)
        return 1

    show_all = not args.packages
    jobs = getattr(args, "jobs", 1)
    no_binary = getattr(args, "no_binary", False)

    if jobs > 1:
        from plume.parallel import parallel_build
        force_set = requested_names if args.force else set()
        total_start = time.monotonic()
        ok, _ = parallel_build(config, ordered, max_workers=jobs, verbose=args.verbose, force_set=force_set)
        if not ok:
            print(f"\n{red('Build failed')} after {fmt_duration(time.monotonic() - total_start)}")
            return 1

    rc, _, _ = _sequential_run(
        ordered, requested_names, show_all, config, args,
        install_package, "Installed", no_binary=no_binary,
    )
    return rc


def cmd_rebuild(args):
    """Clean and rebuild specific packages, building deps only if needed."""
    config, packages = _load(args)
    try:
        requested = filter_packages(packages, args.packages)

        if getattr(args, "no_propagate", False):
            rebuild_set = requested
        else:
            rebuild_set = reverse_deps(requested, packages)
        rebuild_names = {p.full_name for p in rebuild_set}

        selected = expand_with_deps(rebuild_set, packages)
        ordered = resolve_build_order(selected)
    except ValueError as e:
        print(f"{red('plume: error')}: {e}", file=sys.stderr)
        return 1

    total_start = time.monotonic()
    timings = []
    rebuild_count = len(rebuild_set)
    idx = 0
    for pkg in ordered:
        if pkg.full_name in rebuild_names:
            idx += 1
            print(bold(cyan(f"\n[{idx}/{rebuild_count}] {pkg}")))
            print(dim(f"  Cleaning {pkg}..."))
            clean_package(config, pkg)
            ok, elapsed = build_package(config, pkg, verbose=args.verbose)
            timings.append((str(pkg), elapsed))
        elif not is_built(config, pkg):
            ok, elapsed = build_package(config, pkg)
        else:
            continue
        if not ok:
            print(f"\n{red('Rebuild failed for')} {pkg} after {fmt_duration(time.monotonic() - total_start)}")
            return 1

    total = time.monotonic() - total_start
    _print_timing(timings, total)
    print(f"\n{green('Rebuilt')} {rebuild_count} package(s) in {fmt_duration(total)}")
    return 0


def cmd_uninstall(args):
    config, packages = _load(args)
    try:
        requested = filter_packages(packages, args.packages)
    except ValueError as e:
        print(f"{red('plume: error')}: {e}", file=sys.stderr)
        return 1

    if not args.force:
        world = World(config.get("sysroot"))
        installed_pkgs = [p for p in packages if world.contains(p.qualified_name)]
        target_names = {p.full_name for p in requested}
        for pkg in installed_pkgs:
            if pkg.full_name in target_names:
                continue
            for dep in pkg.dependencies:
                if dep in target_names:
                    print(f"{red('plume: error')}: {pkg.qualified_name} depends on {dep}; use --force to uninstall anyway")
                    return 1

    for pkg in requested:
        print(bold(cyan(f"Uninstalling {pkg}")))
        if not uninstall_package(config, pkg):
            return 1
        print(f"  {green('Removed')} {pkg}")
    return 0


def cmd_image(args):
    config, _ = _load(args)
    print(bold("Assembling ISO image"))
    if not assemble_iso(config, verbose=getattr(args, "verbose", False)):
        return 1
    print(f"{green('ISO written to')} {config.get('iso_output')}")
    return 0


def cmd_test(args):
    config, packages = _load(args)
    ordered = resolve_build_order(packages)
    total_start = time.monotonic()
    timings = []
    for i, pkg in enumerate(ordered, 1):
        print(bold(cyan(f"\n[{i}/{len(ordered)}] {pkg}")))
        ok, elapsed = install_package(
            config, pkg, verbose=args.verbose, force=args.force,
            no_binary=getattr(args, "no_binary", False),
        )
        timings.append((str(pkg), elapsed))
        if not ok:
            print(f"\n{red('Build failed')} after {fmt_duration(time.monotonic() - total_start)}", file=sys.stderr)
            return 1

    _print_timing(timings, time.monotonic() - total_start)

    print(bold("\nAssembling ISO image"))
    if not assemble_iso(config, verbose=args.verbose):
        return 1

    print("\n\nRunning tests...\n")
    harness_args = [sys.executable, "tools/test-harness.py"]
    arch_config = (config.get("arch_configs") or {}).get(config.get_arch()) or {}
    if arch_config.get("qemu"):
        harness_args.extend(["--qemu", arch_config["qemu"]])
    if args.verbose:
        harness_args.append("--verbose")
    harness_args.extend(args.tests)
    return subprocess.run(harness_args, cwd=config.project_root).returncode


def cmd_status(args):
    config, packages = _load(args)
    world = World(config.get("sysroot"))
    installed = set(world.read())

    name_width = max((len(p.full_name) for p in packages), default=10)
    print(bold(f"  {'Package':<{name_width}}  {'Built':<7}  {'Installed'}"))
    print(f"  {'─' * (name_width + 22)}")

    for pkg in packages:
        built = green("✓") if is_built(config, pkg) else dim("–")
        inst = green("✓") if pkg.full_name in installed else (dim("n/a") if pkg.is_build_tool else dim("–"))
        tags = ""
        if pkg.is_build_tool:
            tags += dim("  [build-tool]")
        if pkg.supports_live_sources:
            tags += dim("  [live]")
        print(f"  {pkg.full_name:<{name_width}}  {built:<7}  {inst}{tags}")
    return 0


def cmd_validate(args):
    _, packages = _load(args)
    print(f"{green('✓')} All {len(packages)} package(s) valid.")
    return 0


def cmd_world(args):
    config, _ = _load(args)
    entries = World(config.get("sysroot")).read()
    if not entries:
        print("No packages installed.")
    else:
        for entry in entries:
            print(f"  {entry}")
    return 0


def cmd_clean(args):
    config, _ = _load(args)
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
    kernel_pkgs = [p for p in packages if p.name == "kernel"]
    if not kernel_pkgs:
        print(f"{red('plume: error')}: no kernel package found", file=sys.stderr)
        return 1

    from plume.env import get_build_env
    env = get_build_env(config, kernel_pkgs[0])
    os.makedirs(os.path.join(config.get("build_dir"), "obj", "sys", "kernel"), exist_ok=True)
    result = subprocess.run(
        [env["MAKE"], "-B", "-j", env["MAKE_JOBS"], f"BUILD_DIR={config.get('build_dir')}"],
        cwd=env["LIVE_SOURCES"], env=env,
    )
    if result.returncode != 0:
        print(f"{red('plume: error')}: kernel rebuild failed", file=sys.stderr)
        return 1

    return subprocess.run(
        [sys.executable, "tools/merge-compile-commands.py",
         config.get("build_dir"),
         os.path.join(config.get("build_dir"), "compile_commands.json")],
        cwd=config.project_root,
    ).returncode


def main(argv=None):
    parser = argparse.ArgumentParser(prog="plume", description="Plume -- the Archipelago package manager")
    sub = parser.add_subparsers(dest="command")

    build_p = sub.add_parser("build", help="Build packages (does not install to sysroot)")
    build_p.add_argument("packages", nargs="*", help="Packages to build (default: all, supports @set)")
    build_p.add_argument("--config", default=None)
    build_p.add_argument("--verbose", "-v", action="store_true", help="Stream make output instead of capturing")
    build_p.add_argument("--dry-run", "-n", action="store_true", help="Print build order without building")
    build_p.add_argument("--force", "-f", action="store_true", help="Force rebuild even if already built")
    build_p.add_argument("--jobs", "-j", type=int, default=1, help="Number of parallel package builds (default: 1)")

    install_p = sub.add_parser("install", help="Install packages into sysroot (builds if needed)")
    install_p.add_argument("packages", nargs="*", help="Packages to install (default: all, supports @set)")
    install_p.add_argument("--config", default=None)
    install_p.add_argument("--verbose", "-v", action="store_true", help="Stream make output instead of capturing")
    install_p.add_argument("--force", "-f", action="store_true", help="Force rebuild even if already built")
    install_p.add_argument("--jobs", "-j", type=int, default=1, help="Number of parallel package builds (default: 1)")
    install_p.add_argument("--no-binary", action="store_true", help="Build from source even if binary package is cached")

    rebuild_p = sub.add_parser("rebuild", help="Clean and rebuild specific packages")
    rebuild_p.add_argument("packages", nargs="*", help="Packages to rebuild (default: all, supports @set)")
    rebuild_p.add_argument("--config", default=None)
    rebuild_p.add_argument("--verbose", "-v", action="store_true", help="Stream make output instead of capturing")
    rebuild_p.add_argument("--no-propagate", action="store_true", help="Only rebuild named packages, not reverse deps")
    rebuild_p.add_argument("--jobs", "-j", type=int, default=1, help="Number of parallel package builds (default: 1)")

    uninstall_p = sub.add_parser("uninstall", help="Remove packages from sysroot")
    uninstall_p.add_argument("packages", nargs="+", help="Packages to uninstall")
    uninstall_p.add_argument("--config", default=None)
    uninstall_p.add_argument("--force", "-f", action="store_true", help="Uninstall even if other packages depend on it")

    image_p = sub.add_parser("image", help="Assemble ISO image")
    image_p.add_argument("--config", default=None)
    image_p.add_argument("--verbose", "-v", action="store_true", help="Show xorriso and limine output")

    test_p = sub.add_parser("test", help="Build and run tests")
    test_p.add_argument("tests", nargs="*")
    test_p.add_argument("--verbose", action="store_true")
    test_p.add_argument("--force", "-f", action="store_true", help="Force rebuild even if already built")
    test_p.add_argument("--config", default=None)
    test_p.add_argument("--no-binary", action="store_true", help="Build from source even if binary package is cached")

    sub.add_parser("status", help="Show build and install state of all packages").add_argument("--config", default=None)
    sub.add_parser("validate", help="Validate packages.yml and package Makefiles").add_argument("--config", default=None)
    sub.add_parser("clean", help="Remove build artifacts").add_argument("--config", default=None)

    list_p = sub.add_parser("list", help="List packages")
    list_p.add_argument("--config", default=None)
    list_p.add_argument("--tree", action="store_true", help="Show dependency tree")

    sub.add_parser("world", help="Show packages installed in the sysroot").add_argument("--config", default=None)
    sub.add_parser("clangd", help="Regenerate compile_commands.json").add_argument("--config", default=None)

    args = parser.parse_args(argv)
    if not args.command:
        parser.print_help()
        return 1

    commands = {
        "build": cmd_build, "install": cmd_install, "uninstall": cmd_uninstall,
        "rebuild": cmd_rebuild, "image": cmd_image, "test": cmd_test,
        "status": cmd_status, "validate": cmd_validate, "clean": cmd_clean,
        "list": cmd_list, "world": cmd_world, "clangd": cmd_clangd,
    }
    return commands[args.command](args)
