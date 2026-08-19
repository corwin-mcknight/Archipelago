"""Plume CLI -- the Archipelago build coordinator."""

import argparse
import os
import shutil
import subprocess
import sys
import time

from plume.config import Config, is_bare_config_name
from plume.output import bold, green, red, yellow, cyan, dim, fmt_duration, fmt_reason, break_line
from plume.repository import load_packages
from plume.universe import resolve_build_order, filter_packages, expand_with_deps, reverse_deps
from plume.builder import build_needed, build_log_path, orchestrate, sysroot_needs_compose
from plume.image import assemble_image, assemble_sd


def _find_up(relpath):
    """Return *relpath* resolved against cwd or the nearest ancestor where it exists, else None."""
    d = os.getcwd()
    while True:
        candidate = os.path.join(d, relpath)
        if os.path.exists(candidate):
            return candidate
        parent = os.path.dirname(d)
        if parent == d:
            return None
        d = parent


def _all_targets(include_boards=False):
    """Target names from repo/config/*.yaml, walking up to the project root.

    Board targets are named '<arch>^<board>'. By default only arch targets are
    returned -- each already builds its own default board -- so adding a board
    does not lengthen every matrix run. `include_boards` returns the full set.
    """
    cfg_dir = _find_up(os.path.join("repo", "config"))
    if not cfg_dir:
        return []
    names = sorted(f[:-5] for f in os.listdir(cfg_dir) if f.endswith(".yaml"))
    return names if include_boards else [n for n in names if "^" not in n]


def _run_matrix(args, command, include_boards=False):
    """Run a command once per target (--arch all / all-boards), then print a summary."""
    targets = _all_targets(include_boards)
    if not targets:
        print(f"{red('plume: error')}: no configs found under repo/config/", file=sys.stderr)
        return 1

    results = []
    for target in targets:
        print(bold(cyan(f"=== {target} ===")))
        args.arch = target
        start = time.monotonic()
        rc = command(args)
        results.append((target, rc, time.monotonic() - start))
        print()

    width = max(len(t) for t, _, _ in results)
    print(bold("Matrix summary:"))
    for target, rc, elapsed in results:
        mark = green("✓") if rc == 0 else red("✗")
        print(f"  {target:<{width}}  {mark}  {dim(fmt_duration(elapsed))}")
    return 0 if all(rc == 0 for _, rc, _ in results) else 1


def _config_by_name(name):
    """Resolve a target name to repo/config/<name>.yaml, walking up from the
    working directory to find the project root.

    Accepts an arch ('riscv64') or a board target ('riscv64^visionfive2').
    Path separators are rejected so a target name can never escape the config
    directory.
    """
    if not is_bare_config_name(name):
        return None
    return _find_up(os.path.join("repo", "config", f"{name}.yaml"))


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
            print(f"{red('plume: error')}: no config for target '{arch}' (expected repo/config/{arch}.yaml)",
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

    from plume.validate import validate_packages, validate_package_yaml
    for w in validate_package_yaml(raw_data or {}):
        print(f"{yellow('plume: warning')}: {w}", file=sys.stderr)

    packages = load_packages(packages_yml, arch=config.get_arch(), board=config.get_board())

    errors = validate_packages(config, packages)
    if errors:
        for e in errors:
            print(f"{red('plume: error')}: {e}", file=sys.stderr)
        sys.exit(1)

    return config, packages


def _build_graph(packages, requested):
    """Resolve a build request to (ordered graph, compose set, requested names).

    The sysroot is the union of every supported non-tool package, so any
    request touching a system package builds the whole system graph -- cheap
    when fresh -- and recomposes the sysroot coherently. A request naming
    only build tools (the host test lanes) builds just that closure and
    leaves the sysroot alone.
    """
    selected = filter_packages(packages, requested) if requested else []
    tool_only = bool(selected) and all(p.is_build_tool for p in selected)
    system = [p for p in packages if p.supported and not p.is_build_tool]
    base = selected if tool_only else system + selected
    ordered = resolve_build_order(expand_with_deps(base, packages))
    return ordered, ([] if tool_only else system), {p.full_name for p in selected}


def _run_build(config, packages, args):
    """Shared build+compose entry for the build and test commands."""
    requested = getattr(args, "packages", None) or []
    try:
        ordered, compose_set, requested_names = _build_graph(packages, requested)
    except ValueError as e:
        print(f"{red('plume: error')}: {e}", file=sys.stderr)
        return 1

    jobs = getattr(args, "jobs", 1)
    verbose = getattr(args, "verbose", False)
    if verbose and jobs > 1:
        print(f"{yellow('plume: warning')}: --verbose is ignored with --jobs > 1", file=sys.stderr)
        verbose = False

    force_set = set()
    if getattr(args, "force", False):
        force_set = requested_names or {p.full_name for p in ordered}

    if getattr(args, "dry_run", False):
        shown = [p for p in ordered if p.full_name in force_set or build_needed(config, p) is not None]
        print(bold("Build order (dry run):"))
        for i, pkg in enumerate(shown, 1):
            print(f"  [{i}/{len(shown)}] {pkg}{fmt_reason(build_needed(config, pkg))}")
        if not shown:
            print("  nothing to build")
        return 0

    return orchestrate(config, ordered, compose_set, jobs=jobs, force_set=force_set,
                       verbose=verbose, keep_going=getattr(args, "keep_going", False))


def cmd_build(args):
    config, packages = _load(args)
    return _run_build(config, packages, args)


def cmd_image(args):
    config, _ = _load(args)
    print(bold("Assembling boot image"))
    if not assemble_image(config, verbose=getattr(args, "verbose", False)):
        return 1
    print(f"{green('Image written to')} {config.get('image_output')}")
    return 0


def cmd_test(args):
    config, packages = _load(args)
    if _run_build(config, packages, args) != 0:
        return 1

    print(bold("\nAssembling boot image"))
    if not assemble_image(config, verbose=args.verbose):
        return 1

    print("\n\nRunning tests...\n")
    from plume.env import package_obj_dir
    kernel_pkgs = [p for p in packages if p.name == "kernel" and p.category == "sys"]
    harness_args = [
        sys.executable, "tools/test-harness.py",
        "--arch", config.get_arch(),
        "--iso", config.get("image_output"),
        "--artifacts", os.path.join(config.get("build_dir"), "test-artifacts"),
    ]
    # Omit --kernel-elf when no kernel package exists so the harness runs its own fallback lookup;
    # an empty-string Path would resolve to '.' and symbolicate against the working directory.
    if kernel_pkgs:
        harness_args.extend(["--kernel-elf", os.path.join(package_obj_dir(config, kernel_pkgs[0]), "kernel.elf")])
    if config.get("qemu"):
        harness_args.extend(["--qemu", config.get("qemu")])
    if config.get("firmware"):
        harness_args.extend(["--firmware", config.get("firmware")])
    if args.verbose:
        harness_args.append("--verbose")
    harness_args.extend(args.tests)
    return subprocess.run(harness_args, cwd=config.project_root).returncode


def cmd_uboot_test(args):
    """Boot the sysroot through OpenSBI -> U-Boot -> Limine in QEMU and require
    the kernel to come up. This exercises the SD-card boot chain real boards
    use (U-Boot's EFI loader finding /EFI/BOOT on a FAT partition), which the
    EDK2 ISO path in `plume test` never touches."""
    import threading

    config, _ = _load(args)
    if config.get_arch() != "riscv64":
        print(f"{red('plume: error')}: uboot-test is riscv64-only", file=sys.stderr)
        return 1
    uboot = os.path.join(config.get("tools_path"), "u-boot-qemu", "u-boot.bin")
    if not os.path.isfile(uboot):
        print(f"{red('plume: error')}: no U-Boot at {uboot}; run `plume build`", file=sys.stderr)
        return 1

    print(bold("Assembling SD image"))
    sd_image = os.path.join(config.get("build_dir"), "uboot-sd.img")
    if not assemble_sd(config, verbose=args.verbose, output=sd_image):
        return 1

    marker = "boot: initialization complete"
    qemu = config.get("qemu", "qemu-system-riscv64")
    cmd = [qemu, "-M", "virt", "-m", str(config.get("memory", 128)), "-nographic",
           "-kernel", uboot, "-drive", f"file={sd_image},format=raw,if=virtio", "-no-reboot"]
    print(bold("Booting OpenSBI -> U-Boot -> Limine -> kernel"))
    if args.verbose:
        print(dim(" ".join(cmd)))

    proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, errors="replace")
    # The whole chain takes a few seconds; the timer only bounds a hung boot.
    killer = threading.Timer(120, proc.kill)
    killer.start()
    tail, passed = [], False
    try:
        for line in proc.stdout:
            if args.verbose:
                print(line, end="")
            tail.append(line)
            del tail[:-30]
            if marker in line:
                passed = True
                break
    finally:
        killer.cancel()
        proc.kill()
        proc.wait()

    if passed:
        print(f"{green('PASS')}: kernel reached '{marker}' via the U-Boot chain")
        return 0
    print(f"{red('FAIL')}: marker '{marker}' not seen; last output:", file=sys.stderr)
    print("".join(tail), end="", file=sys.stderr)
    return 1


def cmd_status(args):
    config, packages = _load(args)

    name_width = max((len(p.full_name) for p in packages), default=10)
    print(bold(f"  {'Package':<{name_width}}  {'Built'}"))
    print(f"  {'─' * (name_width + 12)}")

    for pkg in packages:
        reason = build_needed(config, pkg)
        built = green("✓") if reason is None else dim("–")
        tags = ""
        if pkg.is_build_tool:
            tags += dim("  [build-tool]")
        if pkg.supports_live_sources:
            tags += dim("  [live]")
        if pkg.arches:
            tags += dim(f"  [{', '.join(pkg.arches)} only]")
        tags += fmt_reason(reason)
        print(f"  {pkg.full_name:<{name_width}}  {built}{tags}")

    system = [p for p in packages if p.supported and not p.is_build_tool]
    stale = sysroot_needs_compose(config, system)
    if stale:
        print(f"\n  Sysroot: {dim('–')}  {dim(f'({stale})')}")
    else:
        print(f"\n  Sysroot: {green('✓')}  {dim(config.get('sysroot'))}")
    return 0


def cmd_clean(args):
    config, _ = _load(args)
    from plume.builder import sysroot_stamp_path
    targets = [os.path.join(config.get("build_dir"), d) for d in ("obj", "tmp", "logs")]
    targets += [config.get("sysroot"), config.get("image_output"), sysroot_stamp_path(config)]
    for p in targets:
        if os.path.isdir(p):
            shutil.rmtree(p)
        elif os.path.exists(p):
            os.remove(p)
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

    from plume.env import get_build_env, package_obj_dir
    env = get_build_env(config, kernel_pkgs[0])
    obj_dir = package_obj_dir(config, kernel_pkgs[0])
    os.makedirs(obj_dir, exist_ok=True)
    result = subprocess.run(
        [env["MAKE"], "-B", "-j", env["MAKE_JOBS"],
         f"BUILD_DIR={config.get('build_dir')}", f"OBJ_DIR={obj_dir}"],
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
        target_name = cfg.target_name()
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
    print(f"default.yaml -> {rel} ({bold(target_name)})")
    return 0


def _one_package(packages, name):
    """Resolve a single-package argument, printing an error and returning None if unknown."""
    try:
        return filter_packages(packages, [name])[0]
    except ValueError as e:
        print(f"{red('plume: error')}: {e}", file=sys.stderr)
        return None


def cmd_shell(args):
    """Open an interactive shell inside a package's build environment."""
    config, packages = _load(args)
    pkg = _one_package(packages, args.package)
    if pkg is None:
        return 1

    from plume.env import get_build_env
    env = get_build_env(config, pkg)
    cwd = env["S"] if os.path.isdir(env["S"]) else env["FILESDIR"]

    print(bold(cyan(f"Entering build environment for {pkg}")))
    for var in ("S", "D", "WORKDIR", "SYSROOT", "FILESDIR"):
        print(f"  {dim(f'{var:<8}=')} {env[var]}")
    print(f"  stages: {dim('$MAKE -f $FILESDIR/Makefile pkg_get_source|pkg_configure|pkg_build|pkg_install')}",
          flush=True)

    shell = os.environ.get("SHELL", "/bin/sh")
    return subprocess.run([shell], env=env, cwd=cwd).returncode


def cmd_deps(args):
    """Show a package's transitive dependencies and reverse dependencies."""
    _, packages = _load(args)
    pkg = _one_package(packages, args.package)
    if pkg is None:
        return 1

    reverse = sorted(p.full_name for p in reverse_deps([pkg], packages) if p.full_name != pkg.full_name)

    if args.tree:
        by_name = {p.full_name: p for p in packages}

        def print_tree(parent, prefix):
            for i, dep_name in enumerate(parent.dependencies):
                last = i == len(parent.dependencies) - 1
                print(f"{prefix}{'└─' if last else '├─'} {dep_name}")
                child = by_name.get(dep_name)
                if child:
                    print_tree(child, prefix + ("   " if last else "│  "))

        print(bold(pkg.full_name) + ("" if pkg.dependencies else dim("  (no dependencies)")))
        print_tree(pkg, "")
    else:
        forward = [p for p in resolve_build_order(expand_with_deps([pkg], packages))
                   if p.full_name != pkg.full_name]
        print(bold("Depends on (build order):") + ("" if forward else " nothing"))
        for p in forward:
            print(f"  {p.full_name}")
    print(bold("Depended on by:") + ("" if reverse else " nothing"))
    for name in reverse:
        print(f"  {name}")
    return 0


def cmd_log(args):
    """Print the persisted build log for a package."""
    config, packages = _load(args)
    pkg = _one_package(packages, args.package)
    if pkg is None:
        return 1
    log_path = build_log_path(config, pkg)
    if not os.path.isfile(log_path):
        print(f"{red('plume: error')}: no build log for {pkg} (never built, or built with --verbose)",
              file=sys.stderr)
        return 1
    print(dim(log_path), file=sys.stderr)
    with open(log_path, "r", encoding="utf-8") as f:
        sys.stdout.write(f.read())
    return 0


def cmd_run(args):
    """Launch the built ISO interactively in QEMU on the active target."""
    config, _ = _load(args)
    arch = config.get_arch()
    qemu = config.get("qemu", f"qemu-system-{arch}")
    iso = config.get("image_output")
    firmware = config.get("firmware")
    if not os.path.exists(iso):
        print(f"{red('plume: error')}: no ISO at {iso}; run `plume image` first", file=sys.stderr)
        return 1
    if firmware and not os.path.exists(firmware):
        print(f"{red('plume: error')}: UEFI firmware missing at {firmware}; run `plume build`",
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
    parser = argparse.ArgumentParser(prog="plume", description="Plume -- the Archipelago build coordinator")
    sub = parser.add_subparsers(dest="command")

    # Target selection, shared by every subcommand.
    target = argparse.ArgumentParser(add_help=False)
    target.add_argument("--config", default=None, help="Path to a target config yaml")
    target.add_argument("--arch", default=None,
                        help="Target: an arch ('riscv64') or board ('riscv64^visionfive2'), resolving "
                             "repo/config/<target>.yaml. 'all' runs every arch at its default board; "
                             "'all-boards' runs every board target too")

    build_p = sub.add_parser("build", parents=[target],
                             help="Build stale packages and compose the sysroot")
    build_p.add_argument("packages", nargs="*",
                         help="Extra packages to build (supports @set); naming only build tools skips the sysroot")
    build_p.add_argument("--verbose", "-v", action="store_true", help="Stream make output instead of capturing")
    build_p.add_argument("--dry-run", "-n", action="store_true", help="Print build order without building")
    build_p.add_argument("--force", "-f", action="store_true",
                         help="Force rebuild (of the named packages, or everything)")
    build_p.add_argument("--jobs", "-j", type=int, default=1, help="Number of parallel package builds (default: 1)")
    build_p.add_argument("--keep-going", "-k", action="store_true",
                         help="Keep building other packages after a failure (skips dependents of the failed one)")

    image_p = sub.add_parser("image", parents=[target], help="Assemble the boot image (ISO or SD, per config)")
    image_p.add_argument("--verbose", "-v", action="store_true", help="Show image-tool and limine output")

    uboot_p = sub.add_parser("uboot-test", parents=[target],
                             help="Boot the U-Boot EFI chain (SD image) in QEMU and check the kernel comes up")
    uboot_p.add_argument("--verbose", "-v", action="store_true", help="Stream the serial console")

    test_p = sub.add_parser("test", parents=[target], help="Build, compose, image, and run tests")
    test_p.add_argument("tests", nargs="*")
    test_p.add_argument("--verbose", action="store_true")
    test_p.add_argument("--force", "-f", action="store_true", help="Force rebuild even if already built")
    test_p.add_argument("--jobs", "-j", type=int, default=1, help="Number of parallel package builds (default: 1)")

    sub.add_parser("status", parents=[target], help="Show build and sysroot state")
    sub.add_parser("clean", parents=[target], help="Remove build artifacts")

    list_p = sub.add_parser("list", parents=[target], help="List packages")
    list_p.add_argument("--tree", action="store_true", help="Show dependency tree")

    sub.add_parser("clangd", parents=[target], help="Regenerate compile_commands.json")

    shell_p = sub.add_parser("shell", parents=[target], help="Open a shell in a package's build environment")
    shell_p.add_argument("package", help="Package to enter, e.g. sys/kernel")

    deps_p = sub.add_parser("deps", parents=[target], help="Show a package's dependencies and reverse dependencies")
    deps_p.add_argument("package", help="Package to inspect, e.g. sys/kernel")
    deps_p.add_argument("--tree", action="store_true", help="Render dependencies as a recursive tree")

    log_p = sub.add_parser("log", parents=[target], help="Print a package's most recent build log")
    log_p.add_argument("package", help="Package whose log to print, e.g. sys/kernel")

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
        "build": cmd_build, "image": cmd_image, "test": cmd_test,
        "status": cmd_status, "clean": cmd_clean, "list": cmd_list,
        "clangd": cmd_clangd, "set-config": cmd_set_config, "run": cmd_run,
        "shell": cmd_shell, "uboot-test": cmd_uboot_test,
        "deps": cmd_deps, "log": cmd_log,
    }
    try:
        selected = getattr(args, "arch", None)
        if selected in ("all", "all-boards"):
            return _run_matrix(args, commands[args.command], include_boards=(selected == "all-boards"))
        return commands[args.command](args)
    except KeyboardInterrupt:
        break_line()
        print(red("Interrupted."), file=sys.stderr)
        return 130
    except Exception:
        break_line()  # tracebacks should not start mid-line
        raise
