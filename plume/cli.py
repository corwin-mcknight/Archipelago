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


def _all_arches():
    """Arch names from repo/config/*.yaml, walking up to the project root."""
    d = os.getcwd()
    while True:
        cfg_dir = os.path.join(d, "repo", "config")
        if os.path.isdir(cfg_dir):
            return sorted(f[:-5] for f in os.listdir(cfg_dir) if f.endswith(".yaml"))
        parent = os.path.dirname(d)
        if parent == d:
            return []
        d = parent


def _run_matrix(args, command):
    """Run a command once per target (--arch all), then print a summary."""
    arches = _all_arches()
    if not arches:
        print(f"{red('plume: error')}: no configs found under repo/config/", file=sys.stderr)
        return 1

    results = []
    for arch in arches:
        print(bold(cyan(f"=== {arch} ===")))
        args.arch = arch
        start = time.monotonic()
        rc = command(args)
        results.append((arch, rc, time.monotonic() - start))
        print()

    width = max(len(a) for a, _, _ in results)
    print(bold("Matrix summary:"))
    for arch, rc, elapsed in results:
        mark = green("✓") if rc == 0 else red("✗")
        print(f"  {arch:<{width}}  {mark}  {dim(fmt_duration(elapsed))}")
    return 0 if all(rc == 0 for _, rc, _ in results) else 1


def _config_by_name(name):
    """Resolve a config name like 'riscv64' to repo/config/<name>.yaml, walking
    up from the working directory to find the project root."""
    d = os.getcwd()
    while True:
        candidate = os.path.join(d, "repo", "config", f"{name}.yaml")
        if os.path.exists(candidate):
            return candidate
        parent = os.path.dirname(d)
        if parent == d:
            return None
        d = parent


def _find_config(override=None, arch=None):
    """Locate the active target config.

    Precedence: the --config override, then --arch (resolved to
    repo/config/<arch>.yaml), then PLUME_CONFIG, then the ./default.yaml
    selection symlink (see `plume set-config`), falling back to
    repo/config/x86_64.yaml when no selection has been made.
    """
    if override:
        return override
    if arch:
        path = _config_by_name(arch)
        if not path:
            print(f"{red('plume: error')}: no config for arch '{arch}' (expected repo/config/{arch}.yaml)",
                  file=sys.stderr)
            sys.exit(1)
        return path
    if "PLUME_CONFIG" in os.environ:
        return os.environ["PLUME_CONFIG"]
    d = os.getcwd()
    while True:
        candidate = os.path.join(d, "default.yaml")
        if os.path.lexists(candidate):
            # A dangling selection symlink is an error, not a silent fallback
            # to x86_64 -- the user selected something that no longer exists.
            if not os.path.exists(candidate):
                print(f"{red('plume: error')}: default.yaml is a dangling symlink: {candidate}", file=sys.stderr)
                sys.exit(1)
            return candidate
        if os.path.isdir(os.path.join(d, "repo", "config")):
            # Project root reached; never walk past it.
            fallback = os.path.join(d, "repo", "config", "x86_64.yaml")
            if os.path.exists(fallback):
                return fallback
            break
        parent = os.path.dirname(d)
        if parent == d:
            break
        d = parent
    print(f"{red('plume: error')}: could not find default.yaml or repo/config/x86_64.yaml", file=sys.stderr)
    sys.exit(1)


def _load(args):
    """Load config and packages, running validation."""
    config_path = _find_config(getattr(args, "config", None), getattr(args, "arch", None))
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
    ordered = resolve_build_order(filter_packages(packages, []))
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
    harness_args = [
        sys.executable, "tools/test-harness.py",
        "--arch", config.get_arch(),
        "--iso", config.get("iso_output"),
        "--kernel-elf", os.path.join(config.get("build_dir"), "obj", "sys", "kernel", "kernel.elf"),
        "--artifacts", os.path.join(config.get("build_dir"), "test-artifacts"),
    ]
    if config.get("qemu"):
        harness_args.extend(["--qemu", config.get("qemu")])
    if config.get("firmware"):
        harness_args.extend(["--firmware", config.get("firmware")])
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
        if pkg.arches:
            tags += dim(f"  [{', '.join(pkg.arches)} only]")
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
        arches = f" [{', '.join(pkg.arches)} only]" if pkg.arches else ""
        print(f"  {pkg.full_name:30s} {pkg.description}{tool}{live}{arches}")
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

    # Editors expect compile_commands.json at the shared build root, not
    # inside the per-arch tree (.vscode points at build/compile_commands.json).
    return subprocess.run(
        [sys.executable, "tools/merge-compile-commands.py",
         config.get("build_dir"),
         os.path.join(config.project_root, "build", "compile_commands.json")],
        cwd=config.project_root,
    ).returncode


def cmd_set_config(args):
    """Point the ./default.yaml selection symlink at a target config."""
    target = os.path.abspath(args.path)
    if not os.path.isfile(target):
        # Bare names resolve to repo/config/<name>.yaml (e.g. `set-config riscv64`).
        by_name = _config_by_name(args.path) if os.sep not in args.path else None
        if not by_name:
            print(f"{red('plume: error')}: no such config: {args.path}", file=sys.stderr)
            return 1
        target = os.path.abspath(by_name)
    try:
        cfg = Config(target)
        arch = cfg.get_arch()
    except Exception as exc:  # malformed yaml / missing config: block
        print(f"{red('plume: error')}: not a loadable config: {exc}", file=sys.stderr)
        return 1
    # Anchor at the project root, not cwd -- a symlink dropped in a
    # subdirectory would never be seen by builds run from the root.
    link = os.path.join(cfg.project_root, "default.yaml")
    rel = os.path.relpath(target, cfg.project_root)
    if os.path.islink(link) or os.path.exists(link):
        os.remove(link)
    os.symlink(rel, link)
    print(f"default.yaml -> {rel} ({bold(arch)})")
    return 0


def cmd_run(args):
    """Launch the built ISO interactively in QEMU on the active target."""
    config, _ = _load(args)
    arch = config.get_arch()
    qemu = config.get("qemu", f"qemu-system-{arch}")
    iso = config.get("iso_output")
    firmware = config.get("firmware")
    if not os.path.exists(iso):
        print(f"{red('plume: error')}: no ISO at {iso}; run `plume image` first", file=sys.stderr)
        return 1
    if firmware and not os.path.exists(firmware):
        print(f"{red('plume: error')}: UEFI firmware missing at {firmware}; run `plume build @system`",
              file=sys.stderr)
        return 1

    # The machine stanza lives in the test harness (single source);
    # load it from there so the two launchers cannot drift.
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "test_harness", os.path.join(config.project_root, "tools", "test-harness.py"))
    harness = importlib.util.module_from_spec(spec)
    sys.modules["test_harness"] = harness  # dataclasses resolve fields via sys.modules
    spec.loader.exec_module(harness)

    cmd = [qemu, "-serial", "stdio", "-no-reboot",
           "-m", str(args.memory or config.get("memory", 128))]
    cmd += harness.machine_args(arch, iso, firmware)
    if args.no_display or firmware:
        # UEFI targets are serial-only; an empty QEMU window helps nobody.
        cmd += ["-display", "none"]
    if args.debug:
        cmd += ["-s", "-S"]

    print(dim(" ".join(cmd)))
    if args.dry_run:
        return 0
    return subprocess.run(cmd, cwd=config.project_root).returncode


def main(argv=None):
    parser = argparse.ArgumentParser(prog="plume", description="Plume -- the Archipelago package manager")
    sub = parser.add_subparsers(dest="command")

    # Target selection, shared by every subcommand.
    target = argparse.ArgumentParser(add_help=False)
    target.add_argument("--config", default=None, help="Path to a target config yaml")
    target.add_argument("--arch", default=None,
                        help="Target architecture (resolves repo/config/<arch>.yaml), or 'all' to run every target")

    build_p = sub.add_parser("build", parents=[target], help="Build packages (does not install to sysroot)")
    build_p.add_argument("packages", nargs="*", help="Packages to build (default: all, supports @set)")
    build_p.add_argument("--verbose", "-v", action="store_true", help="Stream make output instead of capturing")
    build_p.add_argument("--dry-run", "-n", action="store_true", help="Print build order without building")
    build_p.add_argument("--force", "-f", action="store_true", help="Force rebuild even if already built")
    build_p.add_argument("--jobs", "-j", type=int, default=1, help="Number of parallel package builds (default: 1)")

    install_p = sub.add_parser("install", parents=[target], help="Install packages into sysroot (builds if needed)")
    install_p.add_argument("packages", nargs="*", help="Packages to install (default: all, supports @set)")
    install_p.add_argument("--verbose", "-v", action="store_true", help="Stream make output instead of capturing")
    install_p.add_argument("--force", "-f", action="store_true", help="Force rebuild even if already built")
    install_p.add_argument("--jobs", "-j", type=int, default=1, help="Number of parallel package builds (default: 1)")
    install_p.add_argument("--no-binary", action="store_true", help="Build from source even if binary package is cached")

    rebuild_p = sub.add_parser("rebuild", parents=[target], help="Clean and rebuild specific packages")
    rebuild_p.add_argument("packages", nargs="*", help="Packages to rebuild (default: all, supports @set)")
    rebuild_p.add_argument("--verbose", "-v", action="store_true", help="Stream make output instead of capturing")
    rebuild_p.add_argument("--no-propagate", action="store_true", help="Only rebuild named packages, not reverse deps")
    rebuild_p.add_argument("--jobs", "-j", type=int, default=1, help="Number of parallel package builds (default: 1)")

    uninstall_p = sub.add_parser("uninstall", parents=[target], help="Remove packages from sysroot")
    uninstall_p.add_argument("packages", nargs="+", help="Packages to uninstall")
    uninstall_p.add_argument("--force", "-f", action="store_true", help="Uninstall even if other packages depend on it")

    image_p = sub.add_parser("image", parents=[target], help="Assemble ISO image")
    image_p.add_argument("--verbose", "-v", action="store_true", help="Show xorriso and limine output")

    test_p = sub.add_parser("test", parents=[target], help="Build and run tests")
    test_p.add_argument("tests", nargs="*")
    test_p.add_argument("--verbose", action="store_true")
    test_p.add_argument("--force", "-f", action="store_true", help="Force rebuild even if already built")
    test_p.add_argument("--no-binary", action="store_true", help="Build from source even if binary package is cached")

    sub.add_parser("status", parents=[target], help="Show build and install state of all packages")
    sub.add_parser("validate", parents=[target], help="Validate packages.yml and package Makefiles")
    sub.add_parser("clean", parents=[target], help="Remove build artifacts")

    list_p = sub.add_parser("list", parents=[target], help="List packages")
    list_p.add_argument("--tree", action="store_true", help="Show dependency tree")

    sub.add_parser("world", parents=[target], help="Show packages installed in the sysroot")
    sub.add_parser("clangd", parents=[target], help="Regenerate compile_commands.json")

    setconf_p = sub.add_parser("set-config", help="Select the active target config (symlinks ./default.yaml)")
    setconf_p.add_argument("path", help="Config path or bare arch name, e.g. riscv64")

    run_p = sub.add_parser("run", parents=[target], help="Launch the built ISO in QEMU (interactive; use `plume test` for CI)")
    run_p.add_argument("--debug", action="store_true", help="Start a GDB stub (-s -S) and wait for attach")
    run_p.add_argument("--no-display", action="store_true", help="Headless: serial console only")
    run_p.add_argument("--memory", type=int, default=None, help="Guest memory in MiB (default: from config)")
    run_p.add_argument("--dry-run", "-n", action="store_true", help="Print the QEMU command without launching")

    args = parser.parse_args(argv)
    if not args.command:
        parser.print_help()
        return 1

    commands = {
        "build": cmd_build, "install": cmd_install, "uninstall": cmd_uninstall,
        "rebuild": cmd_rebuild, "image": cmd_image, "test": cmd_test,
        "status": cmd_status, "validate": cmd_validate, "clean": cmd_clean,
        "list": cmd_list, "world": cmd_world, "clangd": cmd_clangd,
        "set-config": cmd_set_config, "run": cmd_run,
    }
    if getattr(args, "arch", None) == "all":
        return _run_matrix(args, commands[args.command])
    return commands[args.command](args)
