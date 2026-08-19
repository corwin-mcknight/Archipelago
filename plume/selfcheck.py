"""Self-checks for target qualifier, config overlay, and staleness logic.

Run with `python3 -m plume.selfcheck`. These cover the branchy parts that a
build alone would not exercise: base-config merging, the arch/board
qualifier, content-hash staleness, and compose conflict detection.
Everything else is covered by actually building a target.
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
    k = Package.parse("sys/kernel", data, arch="riscv64", board="virt")
    assert k.qualified_name == "sys/kernel~riscv64^virt", k.qualified_name
    assert k.variant_suffix == "^virt"
    assert k.varies_by_board

    # A package that declares nothing resolves at the arch level and is shared
    # by every board -- the normal case, not a miss.
    l = Package.parse("boot/limine", plain, arch="riscv64", board="virt")
    assert l.qualified_name == "boot/limine~riscv64", l.qualified_name
    assert l.variant_suffix == ""
    assert not l.varies_by_board

    # Declaring the axis is inert on a target that names no board.
    n = Package.parse("sys/kernel", data, arch="riscv64")
    assert n.qualified_name == "sys/kernel~riscv64", n.qualified_name
    assert n.variant_suffix == ""

    # No arch at all (host-side listing) stays unqualified.
    u = Package.parse("sys/kernel", data)
    assert u.qualified_name == "sys/kernel", u.qualified_name


def check_arches_gating_ignores_board():
    """`arches:` gates on the architecture; a board narrows a target, it never
    changes which arch that target is."""
    gated = {"description": "g", "arches": ["riscv64"], "varies_by": ["board"]}
    assert Package.parse("boot/edk2", gated, arch="riscv64", board="visionfive2").supported
    assert not Package.parse("boot/edk2", gated, arch="x86_64", board="pc").supported


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
    workdir and object dir paths for a package that does not vary."""
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

        a = Package.parse("boot/limine", shared, arch="riscv64", board="virt")
        b = Package.parse("boot/limine", shared, arch="riscv64", board="visionfive2")
        assert get_build_env(arch_cfg, a)["WORKDIR"] == get_build_env(board_cfg, b)["WORKDIR"]
        assert package_obj_dir(arch_cfg, a) == package_obj_dir(board_cfg, b)

        # ...and a board-varying package must produce DIFFERENT ones.
        ka = Package.parse("sys/kernel", varying, arch="riscv64", board="virt")
        kb = Package.parse("sys/kernel", varying, arch="riscv64", board="visionfive2")
        assert get_build_env(arch_cfg, ka)["WORKDIR"] != get_build_env(board_cfg, kb)["WORKDIR"]
        assert package_obj_dir(arch_cfg, ka) != package_obj_dir(board_cfg, kb)

        # BOARD reaches only the package that declared the axis; ARCH stays bare.
        assert get_build_env(arch_cfg, ka)["BOARD"] == "virt"
        assert "BOARD" not in get_build_env(arch_cfg, a)
        assert get_build_env(arch_cfg, ka)["ARCH"] == "riscv64"


def check_content_hash_staleness():
    """Staleness is decided by content, not mtime: a touched-but-identical
    file (branch switch) stays fresh; edits, additions, and deletions are
    each reported as changes."""
    import time
    from plume.stamp import is_stale, update, _tree_hashes

    with tempfile.TemporaryDirectory() as root:
        filesdir = os.path.join(root, "files")
        os.makedirs(filesdir)
        makefile = os.path.join(filesdir, "Makefile")
        with open(makefile, "w") as f:
            f.write("all:\n")
        stamp = os.path.join(root, "work", ".plume-stamp")

        _tree_hashes.cache_clear()  # the walk is cached per invocation
        update(stamp, "hash", [filesdir])
        assert is_stale(stamp, "hash", [filesdir]) is None
        assert is_stale(stamp, "other", [filesdir]) == "config changed"

        # mtime-only change: still fresh.
        os.utime(makefile, (time.time() + 60,) * 2)
        _tree_hashes.cache_clear()
        assert is_stale(stamp, "hash", [filesdir]) is None

        # Content change.
        with open(makefile, "a") as f:
            f.write("\t@true\n")
        _tree_hashes.cache_clear()
        assert is_stale(stamp, "hash", [filesdir]) == "source changed: Makefile"

        # New file.
        _tree_hashes.cache_clear()
        update(stamp, "hash", [filesdir])
        open(os.path.join(filesdir, "patch.diff"), "w").close()
        _tree_hashes.cache_clear()
        assert is_stale(stamp, "hash", [filesdir]) == "source added: patch.diff"

        # Deleted file.
        _tree_hashes.cache_clear()
        update(stamp, "hash", [filesdir])
        os.remove(os.path.join(filesdir, "patch.diff"))
        _tree_hashes.cache_clear()
        assert is_stale(stamp, "hash", [filesdir]) == "source removed: patch.diff"


def check_compose_conflict():
    """Two packages claiming one sysroot path is an error at compose time."""
    from plume.builder import _commit

    with tempfile.TemporaryDirectory() as tmp:
        sysroot = os.path.join(tmp, "sysroot")
        os.makedirs(sysroot)
        stagings = {}
        for name in ("boot/aa", "boot/bb"):
            d = os.path.join(tmp, name.replace("/", "-"), "install")
            os.makedirs(os.path.join(d, "boot"))
            open(os.path.join(d, "boot", "shared.bin"), "w").close()
            stagings[name] = d

        owners = {}
        assert _commit("boot/aa", stagings["boot/aa"], sysroot, owners) is True
        assert os.path.isfile(os.path.join(sysroot, "boot", "shared.bin"))
        assert _commit("boot/bb", stagings["boot/bb"], sysroot, owners) is False, "conflict not detected"


CHECKS = [
    check_overlay_merge,
    check_board_excluded_from_build_hash,
    check_bad_base,
    check_base_cannot_escape_config_dir,
    check_qualifier,
    check_arches_gating_ignores_board,
    check_shared_paths_for_non_varying_package,
    check_content_hash_staleness,
    check_compose_conflict,
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
