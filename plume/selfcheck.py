"""Self-checks for target qualifier and config overlay logic.

Run with `python3 -m plume.selfcheck`. These cover the branchy parts that a
build alone would not exercise: base-config merging and the arch/board
qualifier. Everything else is covered by actually building a target.
"""

import os
import sys
import tempfile

from plume.config import Config
from plume.package import Package


def _write(directory, name, body):
    path = os.path.join(directory, f"{name}.yaml")
    with open(path, "w", encoding="utf-8") as f:
        f.write(body)
    return path


def _config_dir(root):
    """Build repo/config/ under *root*; Config derives project_root two levels up."""
    d = os.path.join(root, "repo", "config")
    os.makedirs(d, exist_ok=True)
    return d


def check_overlay_merge():
    with tempfile.TemporaryDirectory() as root:
        d = _config_dir(root)
        _write(d, "riscv64", "config:\n  arch: riscv64\n  board: virt\n  triple: riscv64-unknown-none\n"
                             "  cc: clang\n  memory: 256\n  build_dir: ./build/riscv64\n")
        path = _write(d, "riscv64^visionfive2",
                      "config:\n  base: riscv64\n  board: visionfive2\n  memory: 8192\n")

        cfg = Config(path)
        assert cfg.get_arch() == "riscv64", cfg.get_arch()
        assert cfg.get_board() == "visionfive2", cfg.get_board()
        assert cfg.target_name() == "riscv64^visionfive2", cfg.target_name()
        # Inherited from base
        assert cfg.get("triple") == "riscv64-unknown-none", cfg.get("triple")
        assert cfg.get("cc") == "clang"
        # Overridden by the overlay
        assert cfg.get("memory") == 8192, cfg.get("memory")
        # `base` is consumed by the merge and never reaches the resolved config
        assert cfg.get("base") is None
        # Paths still resolve against project_root
        assert os.path.isabs(cfg.get("build_dir"))


def check_board_excluded_from_build_hash():
    """A board target must share package staleness with its arch, or every
    board rebuilds the world and the cascade buys nothing."""
    with tempfile.TemporaryDirectory() as root:
        d = _config_dir(root)
        base = _write(d, "riscv64", "config:\n  arch: riscv64\n  board: virt\n  triple: riscv64-unknown-none\n")
        board = _write(d, "riscv64^visionfive2", "config:\n  base: riscv64\n  board: visionfive2\n  memory: 8192\n")
        assert Config(base).build_hash == Config(board).build_hash

        # ...but a real toolchain difference must still invalidate.
        other = _write(d, "riscv64^oddball", "config:\n  base: riscv64\n  board: oddball\n  triple: riscv32-unknown-none\n")
        assert Config(base).build_hash != Config(other).build_hash


def check_bad_base():
    with tempfile.TemporaryDirectory() as root:
        d = _config_dir(root)
        missing = _write(d, "x86_64^ghost", "config:\n  base: nonexistent\n  board: ghost\n")
        try:
            Config(missing)
        except ValueError as e:
            assert "not found" in str(e), e
        else:
            raise AssertionError("missing base config should raise")

        _write(d, "a", "config:\n  base: b\n  arch: x86_64\n")
        loop = _write(d, "b", "config:\n  base: a\n  arch: x86_64\n")
        try:
            Config(loop)
        except ValueError as e:
            assert "circular" in str(e), e
        else:
            raise AssertionError("circular base chain should raise")


def check_qualifier():
    data = {"description": "k", "varies_by": ["board"]}
    plain = {"description": "l"}

    # Board-varying package on a board target: qualifier and paths carry the board.
    k = Package.parse("sys/kernel-0.0.1", data, arch="riscv64", board="virt")
    assert k.qualified_name == "sys/kernel-0.0.1~riscv64^virt", k.qualified_name
    assert k.variant_suffix == "^virt"
    assert k.varies_by_board

    # A package that declares nothing resolves at the arch level and is shared
    # by every board -- the normal case, not a miss.
    l = Package.parse("boot/limine-10.0", plain, arch="riscv64", board="virt")
    assert l.qualified_name == "boot/limine-10.0~riscv64", l.qualified_name
    assert l.variant_suffix == ""
    assert not l.varies_by_board

    # Declaring the axis is inert on a target that names no board.
    n = Package.parse("sys/kernel-0.0.1", data, arch="riscv64")
    assert n.qualified_name == "sys/kernel-0.0.1~riscv64", n.qualified_name
    assert n.variant_suffix == ""

    # No arch at all (host-side listing) stays unqualified.
    u = Package.parse("sys/kernel-0.0.1", data)
    assert u.qualified_name == "sys/kernel-0.0.1", u.qualified_name


def check_arches_gating_ignores_board():
    """`arches:` gates on the architecture; a board narrows a target, it never
    changes which arch that target is."""
    gated = {"description": "g", "arches": ["riscv64"], "varies_by": ["board"]}
    assert Package.parse("boot/edk2-1.0", gated, arch="riscv64", board="visionfive2").supported
    assert not Package.parse("boot/edk2-1.0", gated, arch="x86_64", board="pc").supported


def check_world_prefix_survives_board_qualifier():
    """The world file keys on category/name; a board qualifier must not create
    a second entry for the same package."""
    from plume.world import _name_prefix
    assert _name_prefix("sys/kernel-0.0.1~riscv64^virt") == "sys/kernel"
    assert _name_prefix("sys/kernel-0.0.1~riscv64") == "sys/kernel"
    assert _name_prefix("sys/kernel-0.0.1") == "sys/kernel"


def check_base_cannot_escape_config_dir():
    with tempfile.TemporaryDirectory() as root:
        d = _config_dir(root)
        _write(d, "riscv64", "config:\n  arch: riscv64\n  board: virt\n")
        for bad in ("../../evil", "/etc/evil"):
            path = _write(d, "riscv64^escape", f"config:\n  base: {bad}\n  board: escape\n")
            try:
                Config(path)
            except ValueError as e:
                assert "must name a config" in str(e), e
            else:
                raise AssertionError(f"base '{bad}' should be rejected")


def check_shared_paths_for_non_varying_package():
    """The point of the cascade: two boards on one arch must produce IDENTICAL
    workdir, object dir, and binpkg paths for a package that does not vary."""
    from plume.binpkg import binpkg_path
    from plume.env import get_build_env, package_obj_dir

    with tempfile.TemporaryDirectory() as root:
        d = _config_dir(root)
        _write(d, "riscv64", "config:\n  arch: riscv64\n  board: virt\n"
                             "  build_dir: ./build/riscv64\n  tmp_path: ./build/riscv64/tmp\n"
                             "  sysroot: ./build/riscv64/sysroot\n  repo_path: ./repo\n"
                             "  source_dir: ./src\n  tools_path: ./build/tools\n")
        board_path = _write(d, "riscv64^visionfive2",
                            "config:\n  base: riscv64\n  board: visionfive2\n"
                            "  sysroot: ./build/riscv64/boards/visionfive2/sysroot\n")
        arch_cfg, board_cfg = Config(os.path.join(d, "riscv64.yaml")), Config(board_path)

        shared = {"description": "s"}
        varying = {"description": "v", "varies_by": ["board"]}

        a = Package.parse("boot/limine-10.0", shared, arch="riscv64", board="virt")
        b = Package.parse("boot/limine-10.0", shared, arch="riscv64", board="visionfive2")
        assert get_build_env(arch_cfg, a)["WORKDIR"] == get_build_env(board_cfg, b)["WORKDIR"]
        assert package_obj_dir(arch_cfg, a) == package_obj_dir(board_cfg, b)
        assert binpkg_path(arch_cfg, a) == binpkg_path(board_cfg, b)

        # ...and a board-varying package must produce DIFFERENT ones.
        ka = Package.parse("sys/kernel-0.0.1", varying, arch="riscv64", board="virt")
        kb = Package.parse("sys/kernel-0.0.1", varying, arch="riscv64", board="visionfive2")
        assert get_build_env(arch_cfg, ka)["WORKDIR"] != get_build_env(board_cfg, kb)["WORKDIR"]
        assert package_obj_dir(arch_cfg, ka) != package_obj_dir(board_cfg, kb)
        assert binpkg_path(arch_cfg, ka) != binpkg_path(board_cfg, kb)

        # BOARD reaches only the package that declared the axis; ARCH stays bare.
        assert get_build_env(arch_cfg, ka)["BOARD"] == "virt"
        assert "BOARD" not in get_build_env(arch_cfg, a)
        assert get_build_env(arch_cfg, ka)["ARCH"] == "riscv64"


def check_superseded_install_does_not_orphan_files():
    """Replacing an install under a new qualifier must remove the files the old
    one owned and the new one does not -- deleting only the record would strand
    them, owned by nobody and unreachable by uninstall."""
    from plume.manifest import save_installed_manifest, installed_manifest_path, list_installed_manifests

    def manifest(qualified, paths):
        return {"package": "sys/kernel-0.0.1", "qualified_name": qualified,
                "category": "sys", "name": "kernel", "version": "0.0.1",
                "arch": "x86_64", "dependencies": [],
                "files": [{"path": p, "sha256": "", "size": 0} for p in paths]}

    with tempfile.TemporaryDirectory() as sysroot:
        for rel in ("boot/kernel.elf", "usr/include/kernel/dropped.h"):
            os.makedirs(os.path.join(sysroot, os.path.dirname(rel)), exist_ok=True)
            open(os.path.join(sysroot, rel), "w").close()
        save_installed_manifest(manifest("sys/kernel-0.0.1~x86_64", ["boot/kernel.elf", "usr/include/kernel/dropped.h"]), sysroot)

        # The package becomes board-varying and stops shipping dropped.h.
        open(os.path.join(sysroot, "boot", "kernel.elf"), "w").close()
        save_installed_manifest(manifest("sys/kernel-0.0.1~x86_64^pc", ["boot/kernel.elf"]), sysroot)

        assert os.path.isfile(os.path.join(sysroot, "boot/kernel.elf")), "kept file was deleted"
        assert not os.path.exists(os.path.join(sysroot, "usr/include/kernel/dropped.h")), "orphaned file survived"
        assert not os.path.isdir(os.path.join(sysroot, "usr/include/kernel")), "empty dir not pruned"
        names = [m["qualified_name"] for m in list_installed_manifests(sysroot)]
        assert names == ["sys/kernel-0.0.1~x86_64^pc"], names
        assert os.path.isfile(installed_manifest_path(sysroot, "sys/kernel-0.0.1~x86_64^pc"))


def check_incomplete_manifests_never_supersede():
    """Identity drives deletion, so manifests missing category/name must never
    compare equal to each other."""
    from plume.manifest import save_installed_manifest, list_installed_manifests

    def bare(qualified, path):
        return {"qualified_name": qualified, "files": [{"path": path, "sha256": "", "size": 0}]}

    with tempfile.TemporaryDirectory() as sysroot:
        os.makedirs(os.path.join(sysroot, "boot"), exist_ok=True)
        for rel in ("a.bin", "b.bin"):
            open(os.path.join(sysroot, "boot", rel), "w").close()
        save_installed_manifest(bare("x/a-1~x86_64", "boot/a.bin"), sysroot)
        save_installed_manifest(bare("x/b-1~x86_64", "boot/b.bin"), sysroot)

        assert os.path.isfile(os.path.join(sysroot, "boot/a.bin")), "unrelated file deleted"
        assert len(list_installed_manifests(sysroot)) == 2


def check_corrupt_manifest_does_not_break_installs():
    from plume.manifest import save_installed_manifest, installed_manifest_dir, list_installed_manifests

    with tempfile.TemporaryDirectory() as sysroot:
        mdir = installed_manifest_dir(sysroot)
        os.makedirs(mdir, exist_ok=True)
        with open(os.path.join(mdir, "truncated.json"), "w") as f:
            f.write('{"qualified_name": "x/y-1~x86')  # interrupted write

        assert list_installed_manifests(sysroot) == []
        save_installed_manifest(
            {"qualified_name": "x/y-1~x86_64", "category": "x", "name": "y", "files": []}, sysroot)


CHECKS = [
    check_overlay_merge,
    check_board_excluded_from_build_hash,
    check_bad_base,
    check_base_cannot_escape_config_dir,
    check_qualifier,
    check_arches_gating_ignores_board,
    check_world_prefix_survives_board_qualifier,
    check_shared_paths_for_non_varying_package,
    check_superseded_install_does_not_orphan_files,
    check_incomplete_manifests_never_supersede,
    check_corrupt_manifest_does_not_break_installs,
]


def main():
    failed = 0
    for check in CHECKS:
        try:
            check()
            print(f"  ok    {check.__name__}")
        except Exception as e:  # a check that errors is a failure, not a crash
            failed += 1
            print(f"  FAIL  {check.__name__}: {type(e).__name__}: {e}")
    print(f"\n{len(CHECKS) - failed}/{len(CHECKS)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
