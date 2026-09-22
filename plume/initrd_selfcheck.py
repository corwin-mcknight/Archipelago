"""Archive and image regressions that need no target compiler or boot tools."""

from contextlib import redirect_stderr, redirect_stdout
import importlib.util
import io
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
from unittest.mock import patch

from plume.initrd import INITRD_PATH, RUNTIME_ROOT, _archive_name, assemble_initrd, write_archive


def _runtime(root):
    root = Path(root)
    (root / "bootstrap").mkdir(parents=True)
    for name in ("selftest", "elf_loader", "echo"):
        (root / "bootstrap" / f"{name}.elf").write_bytes(name.encode())
    (root / "bin").mkdir()
    (root / "bin/tool.elf").write_bytes(b"ordinary executable")
    (root / "share").mkdir()
    (root / "share/message.txt").write_bytes(b"ordinary data")
    return root


def _boot_sysroot(root):
    root = Path(root)
    (root / "boot").mkdir(parents=True)
    (root / "boot/init.bin").write_bytes(b"init")
    (root / "boot/kernel.elf").write_bytes(b"kernel")
    (root / "limine.conf").write_text("test config\n")
    _runtime(root / RUNTIME_ROOT)
    return root


def _fails(action, exception=(ValueError, OSError)):
    try:
        action()
    except exception:
        return
    raise AssertionError("invalid input accepted")


def check_initrd_deterministic():
    with tempfile.TemporaryDirectory() as tmp:
        root = _runtime(Path(tmp) / "runtime")
        output = Path(tmp) / "initrd.tar"
        (root / "bootstrap/elf_loader.elf").chmod(0o755)
        # Exercise both the ustar prefix field and lexical ordering around a
        # directory whose emitted header has a trailing slash.
        (root / "bootstrap-extra").write_bytes(b"lexical neighbor")
        long_dir = root / ("d" * 90)
        long_dir.mkdir()
        (long_dir / ("f" * 90)).write_bytes(b"long path")
        # The optional slash in a directory header is not part of its path.
        edge_dir = root / ("d" * 100) / ("d" * 54) / ("d" * 99)
        edge_dir.mkdir(parents=True)
        write_archive(root, output)
        expected = output.read_bytes()
        with tarfile.open(output, "r:") as archive:
            entries = archive.getmembers()
            names = [entry.name for entry in entries]
            assert names == sorted(names), names
            assert "bootstrap/elf_loader.elf" in names
            assert "bin/tool.elf" in names
            assert archive.extractfile("share/message.txt").read() == b"ordinary data"
            assert archive.extractfile("d" * 90 + "/" + "f" * 90).read() == b"long path"
            for entry in entries:
                assert entry.isfile() or entry.isdir()
                assert (entry.uid, entry.gid, entry.mtime, entry.uname, entry.gname) == (0, 0, 0, "", "")
                expected_mode = 0o755 if entry.isdir() or entry.name == "bootstrap/elf_loader.elf" else 0o644
                assert entry.mode == expected_mode
                assert not entry.pax_headers
        assert expected[257:265] == b"ustar\x0000"
        # Host mtimes and irrelevant permission bits do not affect the bytes.
        for path in root.rglob("*"):
            os.utime(path, (12345678, 12345678))
            if path.is_file():
                path.chmod(0o700 if path.name == "elf_loader.elf" else 0o600)
        write_archive(root, output)
        assert output.read_bytes() == expected


def check_initrd_refresh_and_missing_inputs():
    with tempfile.TemporaryDirectory() as tmp:
        root = _boot_sysroot(Path(tmp) / "sysroot")
        assemble_initrd(root)
        output = root / INITRD_PATH
        before = output.read_bytes()
        runtime = root / RUNTIME_ROOT
        (runtime / "share/message.txt").write_bytes(b"new content")
        (runtime / "bin/tool.elf").unlink()
        assemble_initrd(root)
        assert before != output.read_bytes()
        with tarfile.open(output, "r:") as archive:
            assert "bin/tool.elf" not in archive.getnames()
            assert archive.extractfile("share/message.txt").read() == b"new content"
        for required in (root / "boot/init.bin", runtime / "bootstrap/elf_loader.elf"):
            contents = required.read_bytes()
            required.unlink()
            _fails(lambda: assemble_initrd(root))
            required.write_bytes(contents)
        shutil.rmtree(runtime)
        _fails(lambda: assemble_initrd(root))


def check_initrd_rejects_unsupported_inputs():
    for name in ("", "/absolute", "../escape", "a/../b", "./a", "a//b", "a/", "a\\b", "a\nb", "caf\u00e9", "a" * 256):
        _fails(lambda: _archive_name(name))
    with tempfile.TemporaryDirectory() as tmp:
        runtime = _runtime(Path(tmp) / "runtime")
        output = Path(tmp) / "initrd.tar"
        write_archive(runtime, output)
        expected = output.read_bytes()
        bad = runtime / "bad"
        for target in (runtime / "bootstrap/elf_loader.elf", runtime / "bootstrap", runtime / "missing"):
            bad.symlink_to(target)
            _fails(lambda: write_archive(runtime, output))
            bad.unlink()
        os.mkfifo(bad)
        _fails(lambda: write_archive(runtime, output))
        bad.unlink()
        # Legal length alone is insufficient: ustar's unsplit name field is 100 bytes.
        bad = runtime / ("n" * 101)
        bad.write_bytes(b"unrepresentable")
        _fails(lambda: write_archive(runtime, output))
        bad.unlink()
        with patch("plume.initrd.MAX_ARCHIVE_SIZE", 1024):
            _fails(lambda: write_archive(runtime, output))
        assert output.read_bytes() == expected, "failed assembly replaced a valid archive"


def check_initrd_bootstrap_policy():
    with tempfile.TemporaryDirectory() as tmp:
        runtime = _runtime(Path(tmp) / "runtime")
        output = Path(tmp) / "initrd.tar"
        for name in ("init.elf", "Echo.elf", "bad-name.elf", "data.txt", "a" * 32 + ".elf"):
            bad = runtime / "bootstrap" / name
            bad.write_bytes(b"payload")
            _fails(lambda: write_archive(runtime, output))
            bad.unlink()
        bad = runtime / "bootstrap/empty.elf"
        bad.touch()
        _fails(lambda: write_archive(runtime, output))
        bad.unlink()
        bad = runtime / "bootstrap/nested"
        bad.mkdir()
        _fails(lambda: write_archive(runtime, output))
        bad.rmdir()
        for index in range(6):
            (runtime / "bootstrap" / f"service{index}.elf").write_bytes(b"payload")
        write_archive(runtime, output)  # Eight services plus the loader fit.
        (runtime / "bootstrap/one_more.elf").write_bytes(b"payload")
        _fails(lambda: write_archive(runtime, output))


def check_initrd_package_recomposition():
    """An ordinary rebuild must retire outputs removed from pkg_install."""
    from plume.builder import build_needed, orchestrate
    from plume.config import Config
    from plume.env import get_build_env
    from plume.package import Package
    from plume.stamp import _tree_hashes

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        config_dir = root / "repo/config"
        config_dir.mkdir(parents=True)
        config_path = config_dir / "test.yaml"
        config_path.write_text("config:\n  arch: x86_64\n  sysroot: ./sysroot\n  build_dir: ./build\n"
                               "  tmp_path: ./build/tmp\n  tools_path: ./build/tools\n"
                               "  source_dir: ./src\n  repo_path: ./repo\n")
        config = Config(config_path)
        package = Package.parse("sys/runtime", {}, arch="x86_64", board="pc")
        package_dir = root / "repo/packages/sys/runtime"
        package_dir.mkdir(parents=True)
        makefile = package_dir / "Makefile"
        common = ("pkg_get_source pkg_configure pkg_build:\n\t@true\n"
                  "pkg_install:\n\tmkdir -p $(D)/boot $(D)/usr/share/initrd/bootstrap\n"
                  "\tprintf init > $(D)/boot/init.bin\n"
                  "\tprintf loader > $(D)/usr/share/initrd/bootstrap/elf_loader.elf\n")
        makefile.write_text(common + "\tprintf old > $(D)/usr/share/initrd/bootstrap/old.elf\n")
        with redirect_stdout(io.StringIO()):
            assert orchestrate(config, [package], [package]) == 0
            assemble_initrd(config.get("sysroot"))
            makefile.write_text(common + "\tprintf new > $(D)/usr/share/initrd/bootstrap/new.elf\n")
            _tree_hashes.cache_clear()
            assert orchestrate(config, [package], [package]) == 0
            assemble_initrd(config.get("sysroot"))
        with tarfile.open(Path(config.get("sysroot")) / INITRD_PATH, "r:") as archive:
            assert "bootstrap/new.elf" in archive.getnames()
            assert "bootstrap/old.elf" not in archive.getnames()

        # A failed install leaves a nonempty staging directory but no valid
        # package output. Reverting the input must not revive the old stamp.
        successful_inputs = makefile.read_text()
        makefile.write_text(common + "\tprintf partial > $(D)/usr/share/initrd/bootstrap/partial.elf\n\tfalse\n")
        _tree_hashes.cache_clear()
        with redirect_stdout(io.StringIO()):
            assert orchestrate(config, [package], [package]) == 1
        staging = Path(get_build_env(config, package)["D"])
        assert (staging / RUNTIME_ROOT / "bootstrap/partial.elf").is_file()
        assert not (staging / RUNTIME_ROOT / "bootstrap/new.elf").exists()
        makefile.write_text(successful_inputs)
        _tree_hashes.cache_clear()
        assert build_needed(config, package) is not None, "source revert trusted a failed rebuild"
        with redirect_stdout(io.StringIO()):
            assert orchestrate(config, [package], [package]) == 0
            assemble_initrd(config.get("sysroot"))
        assert build_needed(config, package) is None
        with tarfile.open(Path(config.get("sysroot")) / INITRD_PATH, "r:") as archive:
            assert "bootstrap/new.elf" in archive.getnames()
            assert "bootstrap/partial.elf" not in archive.getnames()


def check_initrd_image_paths():
    """ISO and the direct SD entrypoint use the same packed boot-only tree."""
    from plume.image import assemble_iso, assemble_sd

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        sysroot = _boot_sysroot(root / "sysroot")
        tools = root / "tools/limine-tools"
        tools.mkdir(parents=True)
        (tools / "BOOTRISCV64.EFI").write_bytes(b"efi")

        class Config:
            def get(self, key, default=None):
                return {"sysroot": str(sysroot), "tools_path": str(root / "tools"),
                        "image_output": str(root / "image.iso"),
                        "image": {"efi_boot": "boot/efi.bin"}}.get(key, default)

            def get_arch(self):
                return "riscv64"

        seen = []

        def run(argv, **kwargs):
            if argv[0] == "xorriso":
                boot_root = Path(argv[argv.index("-o") - 1])
            elif argv[:3] == ["mcopy", "-s", "-b"]:
                boot_root = Path(argv[3]).parent
            else:
                return subprocess.CompletedProcess(argv, 0, "")
            assert sorted(path.name for path in boot_root.iterdir()) == ["boot", "limine.conf"]
            assert sorted(path.name for path in (boot_root / "boot").iterdir()) == ["init.bin", "initrd.tar", "kernel.elf"]
            with tarfile.open(boot_root / INITRD_PATH, "r:") as archive:
                assert "bootstrap/echo.elf" in archive.getnames()
                assert "share/message.txt" in archive.getnames()
            seen.append(argv[0])
            return subprocess.CompletedProcess(argv, 0, "")

        with patch("plume.image.subprocess.run", side_effect=run):
            assert assemble_iso(Config())
            assert assemble_sd(Config(), output=str(root / "sd.img"))
        assert seen == ["xorriso", "mcopy"]
        (sysroot / "boot/init.bin").unlink()
        with patch("plume.image.subprocess.run") as run, redirect_stderr(io.StringIO()):
            assert not assemble_iso(Config())
            assert not assemble_sd(Config(), output=str(root / "sd.img"))
            run.assert_not_called()


def check_initrd_boot_configs():
    project = Path(__file__).resolve().parent.parent
    spec = importlib.util.spec_from_file_location("netboot", project / "tools/netboot.py")
    netboot = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(netboot)
    configurations = [path.read_text() for path in (project / "src/boot/limine-config").glob("*.conf")]
    assert len(configurations) >= 2
    configurations.append(netboot.LIMINE_CONF)
    for config in configurations:
        modules = [line.strip().split(": ", 1) for line in config.splitlines() if line.strip().startswith("module_")]
        assert [value for key, value in modules if key == "module_string"] == ["init", "initrd"]
        assert [value.rsplit("/", 1)[-1] for key, value in modules if key == "module_path"] == ["init.bin", "initrd.tar"]
    assert ("boot/initrd.tar", "initrd.tar") in netboot.FILES
    assert all(not source.endswith(".elf") or source == "boot/kernel.elf" for source, _ in netboot.FILES)

    # Missing image contents must never reuse an old TFTP artifact, and a
    # successful refresh drops obsolete loose userspace files.
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "sd.img").write_bytes(bytes(512))
        (root / "boot").mkdir()
        (root / "boot/initrd.tar").write_bytes(b"old archive")
        (root / "boot/echo.elf").write_bytes(b"obsolete")

        def extract(argv, **kwargs):
            staging = Path(argv[-1])
            for source, _ in netboot.FILES:
                if source in ("limine-netboot.conf", "boot/initrd.tar"):
                    continue
                target = staging / source
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(b"new payload")

        with patch.object(netboot.subprocess, "run", side_effect=extract):
            _fails(lambda: netboot.prepare_artifacts(root, "127.0.0.1"))
        assert (root / "boot/initrd.tar").read_bytes() == b"old archive"

        def extract_complete(argv, **kwargs):
            extract(argv, **kwargs)
            (Path(argv[-1]) / "boot/initrd.tar").write_bytes(b"new archive")

        with patch.object(netboot.subprocess, "run", side_effect=extract_complete):
            netboot.prepare_artifacts(root, "127.0.0.1")
        assert (root / "boot/initrd.tar").read_bytes() == b"new archive"
        assert not (root / "boot/echo.elf").exists()
        assert (root / "netboot.stamp").is_file()


CHECKS = [
    check_initrd_deterministic,
    check_initrd_refresh_and_missing_inputs,
    check_initrd_rejects_unsupported_inputs,
    check_initrd_bootstrap_policy,
    check_initrd_package_recomposition,
    check_initrd_image_paths,
    check_initrd_boot_configs,
]
