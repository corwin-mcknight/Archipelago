"""Behavior checks for dependency content and incremental consumer relinking."""

from contextlib import redirect_stdout
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

import yaml

from plume.builder import build_needed, build_package, orchestrate
from plume.config import Config
from plume.env import get_build_env
from plume.repository import load_packages
from plume.stamp import STAMP_NAME, _tree_hashes


def _fixture(root):
    root = Path(root).resolve()
    config_path = root / "repo/config/x86_64.yaml"
    config_path.parent.mkdir(parents=True)
    config_path.write_text(yaml.safe_dump({"config": {
        "arch": "x86_64", "cc": shutil.which("clang"), "cxx": shutil.which("clang++"),
        "ar": shutil.which("llvm-ar") or shutil.which("ar"), "make": shutil.which("gmake") or shutil.which("make"),
        "build_dir": "./build/x86_64", "tmp_path": "./build/x86_64/tmp",
        "sysroot": "./build/x86_64/sysroot", "tools_path": "./build/tools",
        "source_dir": "./src", "repo_path": "./repo",
    }}))
    manifest = {
        "lib/value": {"supports_live_sources": True, "live_source_path": "lib/value"},
        "sys/app": {"supports_live_sources": True, "live_source_path": "sys/app",
                    "dependencies": ["lib/value"]},
    }
    manifest_path = root / "repo/packages.yml"
    manifest_path.write_text(yaml.safe_dump(manifest, sort_keys=False))
    for name, output, destination in (("lib/value", "libvalue.a", "usr/lib"), ("sys/app", "app", "usr/bin")):
        source = root / "src" / name
        source.mkdir(parents=True)
        recipe = root / "repo/packages" / name / "Makefile"
        recipe.parent.mkdir(parents=True)
        recipe.write_text('pkg_get_source pkg_configure:\n\t@true\n'
                          'pkg_build:\n\t$(MAKE) -C $(LIVE_SOURCES)\n'
                          f'pkg_install:\n\tmkdir -p $(D)/{destination}\n'
                          f'\tcp $(OBJ_DIR)/{output} $(D)/{destination}/{output}\n')
        if name == "lib/value":
            with recipe.open("a") as stream:
                stream.write('\tmkdir -p $(D)/usr/include\n\tcp -p $(LIVE_SOURCES)/value.h $(D)/usr/include/value.h\n')
    (root / "src/lib/value/value.c").write_text("int value(void) { return 1; }\n")
    (root / "src/lib/value/value.h").write_text("int value(void);\n#define BIAS 0\n")
    (root / "src/lib/value/Makefile").write_text(
        'all: $(OBJ_DIR)/libvalue.a\n'
        '$(OBJ_DIR)/value.o: value.c\n\tmkdir -p $(OBJ_DIR)\n\t$(CC) -c $< -o $@\n'
        '$(OBJ_DIR)/libvalue.a: $(OBJ_DIR)/value.o\n\t$(AR) rcs $@ $<\n')
    (root / "src/sys/app/main.c").write_text("#include <value.h>\nint main(void) { return value() + BIAS; }\n")
    (root / "src/sys/app/Makefile").write_text(
        'all: $(OBJ_DIR)/app\n'
        '$(OBJ_DIR)/main.o: main.c\n\tmkdir -p $(OBJ_DIR)\n'
        '\t$(CC) -MMD -MP -I$(SYSROOT)/usr/include -c $< -o $@\n'
        '$(OBJ_DIR)/app: $(OBJ_DIR)/main.o $(SYSROOT)/usr/lib/libvalue.a\n\t$(CC) $^ -o $@\n'
        '-include $(OBJ_DIR)/main.d\n')
    return Config(config_path), load_packages(manifest_path, arch="x86_64")


def _build(config, packages, **kwargs):
    _tree_hashes.cache_clear()
    with redirect_stdout(io.StringIO()) as output:
        result = orchestrate(config, packages, packages, **kwargs)
    assert result == 0, output.getvalue()


def _age_initial_build(root):
    # Separate Make timestamps without sleeps, including hosts with whole-second Make comparisons.
    initial = time.time_ns() - 10_000_000_000
    for tree in (Path(root) / "src", Path(root) / "build"):
        for path in tree.rglob("*"):
            if path.is_file():
                timestamp = initial - 10_000_000_000 if tree.name == "src" else initial
                os.utime(path, ns=(timestamp, timestamp))


def _check_dependency_relink(jobs):
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary).resolve()
        config, packages = _fixture(root)
        library, app = packages
        _build(config, packages, jobs=jobs)
        executable = Path(config.get("sysroot")) / "usr/bin/app"
        assert subprocess.run([executable]).returncode == 1
        _age_initial_build(root)
        obj = Path(get_build_env(config, app)["OBJ_DIR"]) / "main.o"
        previous_object = (obj.read_bytes(), obj.stat().st_mtime_ns)
        (root / "src/lib/value/value.c").write_text("int value(void) { return 2; }\n")
        _build(config, packages, jobs=jobs)
        assert subprocess.run([executable]).returncode == 2, "unchanged consumer did not relink changed library"
        assert (obj.read_bytes(), obj.stat().st_mtime_ns) == previous_object, "dependency change discarded consumer object"
        assert build_needed(config, app) is None, "successful consumer rebuild remained stale"


def check_dependency_relink_serial():
    _check_dependency_relink(1)


def check_dependency_relink_parallel():
    _check_dependency_relink(2)


def check_equal_timestamp_dependency_relinks_without_recompiling():
    with tempfile.TemporaryDirectory() as root:
        config, packages = _fixture(root)
        library, app = packages
        _build(config, packages)
        env = get_build_env(config, app)
        executable = Path(config.get("sysroot")) / "usr/bin/app"
        obj = Path(env["OBJ_DIR"]) / "main.o"
        previous = (obj.read_bytes(), obj.stat().st_mtime_ns)
        library_env = get_build_env(config, library)
        # Produce changed content, then reproduce a tied prerequisite timestamp deterministically.
        source = Path(root) / "src/lib/value/value.c"
        source.write_text("int value(void) { return 2; }\n")
        recipe = Path(library_env["FILESDIR"]) / "Makefile"
        recipe.write_text(recipe.read_text().replace('$(MAKE) -C', '$(MAKE) -B -C'))
        _tree_hashes.cache_clear()
        with redirect_stdout(io.StringIO()):
            ok, _ = build_package(config, library)
        assert ok
        archive = Path(library_env["D"]) / "usr/lib/libvalue.a"
        linked = Path(env["OBJ_DIR"]) / "app"
        timestamp = linked.stat().st_mtime_ns
        os.utime(archive, ns=(timestamp, timestamp))
        _build(config, packages, jobs=2)
        assert subprocess.run([executable]).returncode == 2, "equal-timestamp archive did not trigger relink"
        assert (obj.read_bytes(), obj.stat().st_mtime_ns) == previous, "equal-timestamp relink recompiled consumer"
        assert build_needed(config, app) is None


def check_direct_build_rejects_unavailable_dependency():
    with tempfile.TemporaryDirectory() as root:
        config, packages = _fixture(root)
        library, app = packages
        _build(config, packages)
        library_stamp = Path(get_build_env(config, library)["WORKDIR"]) / STAMP_NAME
        library_stamp.unlink()
        env = get_build_env(config, app)
        output = Path(env["D"]) / "usr/bin/app"
        previous = (output.read_bytes(), output.stat().st_mtime_ns)
        with redirect_stdout(io.StringIO()):
            ok, _ = build_package(config, app)
        assert not ok, "direct build published success with unavailable prerequisite"
        assert (output.read_bytes(), output.stat().st_mtime_ns) == previous


def check_preserved_timestamp_header_recompiles_consumer():
    with tempfile.TemporaryDirectory() as root:
        config, packages = _fixture(root)
        _build(config, packages)
        header = Path(root) / "src/lib/value/value.h"
        previous = header.stat()
        header.write_text("int value(void);\n#define BIAS 5\n")
        os.utime(header, ns=(previous.st_atime_ns, previous.st_mtime_ns))
        _build(config, packages)
        executable = Path(config.get("sysroot")) / "usr/bin/app"
        assert subprocess.run([executable]).returncode == 6, "changed header with preserved mtime was not recompiled"


def check_dependency_status_tracks_content_without_mtimes():
    with tempfile.TemporaryDirectory() as root:
        config, packages = _fixture(root)
        library, app = packages
        _build(config, packages)
        archive = Path(get_build_env(config, library)["D"]) / "usr/lib/libvalue.a"
        original = archive.stat()
        os.utime(archive, ns=(original.st_atime_ns, original.st_mtime_ns + 10_000_000_000))
        assert build_needed(config, app) is None, "dependency timestamp churn marked consumer stale"
        archive.write_bytes(b"different installed content")
        os.utime(archive, ns=(original.st_atime_ns, original.st_mtime_ns))
        assert build_needed(config, app) is not None, "dependency content change with preserved mtime was missed"


def check_identical_dependency_rebuild_preserves_consumer():
    with tempfile.TemporaryDirectory() as root:
        config, packages = _fixture(root)
        library, app = packages
        _build(config, packages)
        output = Path(get_build_env(config, app)["D"]) / "usr/bin/app"
        previous = (output.read_bytes(), output.stat().st_mtime_ns)
        _build(config, packages, force_set={library.full_name})
        assert (output.read_bytes(), output.stat().st_mtime_ns) == previous, "identical dependency rebuild reinstalled consumer"
        assert build_needed(config, app) is None


def check_dependency_membership_and_missing_success():
    with tempfile.TemporaryDirectory() as root:
        config, packages = _fixture(root)
        library, app = packages
        _build(config, packages)
        env = get_build_env(config, library)
        extra = Path(env["D"]) / "extra"
        extra.write_text("new dependency output")
        assert build_needed(config, app) is not None, "added dependency output was missed"
        extra.unlink()
        assert build_needed(config, app) is None
        archive = Path(env["D"]) / "usr/lib/libvalue.a"
        previous = archive.stat().st_mode
        archive.chmod(previous ^ 0o111)
        assert build_needed(config, app) is not None, "dependency executable-mode change was missed"
        archive.chmod(previous)
        link = Path(env["D"]) / "alias"
        link.symlink_to("usr/lib/libvalue.a")
        assert build_needed(config, app) is not None, "dependency symlink was missed"
        link.unlink()
        assert build_needed(config, app) is None
        stamp = Path(env["WORKDIR"]) / STAMP_NAME
        stamp.unlink()
        assert build_needed(config, app) is not None, "dependency without successful build was accepted"


def check_legacy_consumer_stamp_rebuilds_once():
    with tempfile.TemporaryDirectory() as root:
        config, packages = _fixture(root)
        library, app = packages
        _build(config, packages)
        stamp = Path(get_build_env(config, app)["WORKDIR"]) / STAMP_NAME
        record = json.loads(stamp.read_text())
        del record["dependencies"]
        stamp.write_text(json.dumps(record))
        assert build_needed(config, app) is not None, "legacy consumer stamp omitted dependency migration"
        assert build_needed(config, library) is None, "dependency-free package was unnecessarily invalidated"
        _build(config, packages)
        assert build_needed(config, app) is None


def check_malformed_dependency_record_rebuilds():
    with tempfile.TemporaryDirectory() as root:
        config, packages = _fixture(root)
        _build(config, packages)
        app = packages[1]
        stamp = Path(get_build_env(config, app)["WORKDIR"]) / STAMP_NAME
        record = json.loads(stamp.read_text())
        for invalid in (None, [], "damaged"):
            record["dependencies"] = invalid
            stamp.write_text(json.dumps(record))
            assert build_needed(config, app) is not None, "malformed dependency stamp was trusted"


def check_failed_dependency_does_not_publish_consumer():
    with tempfile.TemporaryDirectory() as root:
        config, packages = _fixture(root)
        library, app = packages
        _build(config, packages)
        output = Path(get_build_env(config, app)["D"]) / "usr/bin/app"
        previous = (output.read_bytes(), output.stat().st_mtime_ns)
        recipe = Path(get_build_env(config, library)["FILESDIR"]) / "Makefile"
        recipe.write_text('pkg_get_source pkg_configure pkg_build:\n\t@true\npkg_install:\n\t@false\n')
        for jobs in (1, 2):
            _tree_hashes.cache_clear()
            with redirect_stdout(io.StringIO()):
                result = orchestrate(config, packages, packages, jobs=jobs, keep_going=True)
            assert result == 1, "failed dependency was accepted"
            assert (output.read_bytes(), output.stat().st_mtime_ns) == previous, "consumer ran after dependency failure"
            assert build_needed(config, app) is not None


def check_board_dependency_uses_selected_staging():
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary).resolve()
        config, _ = _fixture(root)
        manifest_path = root / "repo/packages.yml"
        manifest = yaml.safe_load(manifest_path.read_text())
        manifest["lib/value"]["varies_by"] = ["board"]
        manifest_path.write_text(yaml.safe_dump(manifest, sort_keys=False))
        config.config["board"] = "pc"
        packages = load_packages(manifest_path, arch="x86_64", board="pc")
        _build(config, packages)
        _age_initial_build(root)
        config.config["board"] = "other"
        other = load_packages(manifest_path, arch="x86_64", board="other")
        (root / "src/lib/value/value.c").write_text("int value(void) { return 2; }\n")
        _build(config, other, jobs=2)
        executable = Path(config.get("sysroot")) / "usr/bin/app"
        assert subprocess.run([executable]).returncode == 2, "consumer used previous board dependency"
        assert build_needed(config, other[1]) is None
        config.config["board"] = "pc"
        assert build_needed(config, packages[1]) is not None, "consumer trusted another board's dependency record"


def _native_fixture(root, *, native_consumer=False):
    root = Path(root).resolve()
    config, _ = _fixture(root)
    manifest = {"tools/producer": {"is_build_tool": True},
                "sys/consumer": {"dependencies": ["tools/producer"], "is_build_tool": native_consumer}}
    manifest_path = root / "repo/packages.yml"
    manifest_path.write_text(yaml.safe_dump(manifest, sort_keys=False))
    packages = load_packages(manifest_path, arch="x86_64")
    for name in manifest:
        recipe = root / "repo/packages" / name / "Makefile"
        recipe.parent.mkdir(parents=True)
        destination = "$(TOOL_INSTALL)/consumer" if native_consumer else "$(D)"
        install = ('mkdir -p $(TOOL_INSTALL)/producer\n\tcp $(FILESDIR)/value $(TOOL_INSTALL)/producer/value'
                   if name == "tools/producer" else
                   f'mkdir -p {destination}\n\tcp $(TOOLS_PATH)/producer/value {destination}/value')
        recipe.write_text('pkg_get_source pkg_configure pkg_build:\n\t@true\npkg_install:\n\t' + install + '\n')
    (root / "repo/packages/tools/producer/value").write_text("one")
    return config, packages


def check_native_dependency_rebuild_recomposes_sysroot():
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary).resolve()
        config, packages = _native_fixture(root)
        value = root / "repo/packages/tools/producer/value"
        def build():
            _tree_hashes.cache_clear()
            with redirect_stdout(io.StringIO()) as log:
                result = orchestrate(config, packages, [packages[1]], jobs=2)
            assert result == 0, log.getvalue()
        build()
        installed = Path(config.get("sysroot")) / "value"
        assert installed.read_text() == "one"
        value.write_text("two")
        build()
        assert installed.read_text() == "two", "native dependency rebuild left old composed output"


def check_native_dependencies_remain_shared_across_targets():
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary).resolve()
        config, packages = _native_fixture(root, native_consumer=True)
        with redirect_stdout(io.StringIO()):
            assert orchestrate(config, packages, []) == 0
        config.config["arch"] = "riscv64"
        other = load_packages(root / "repo/packages.yml", arch="riscv64")
        assert build_needed(config, other[1]) is None, "native dependency record varied with target architecture"


CHECKS = [check_dependency_relink_serial, check_dependency_relink_parallel,
          check_equal_timestamp_dependency_relinks_without_recompiling, check_direct_build_rejects_unavailable_dependency,
          check_preserved_timestamp_header_recompiles_consumer,
          check_dependency_status_tracks_content_without_mtimes, check_identical_dependency_rebuild_preserves_consumer,
          check_dependency_membership_and_missing_success, check_legacy_consumer_stamp_rebuilds_once,
          check_malformed_dependency_record_rebuilds,
          check_failed_dependency_does_not_publish_consumer, check_board_dependency_uses_selected_staging,
          check_native_dependency_rebuild_recomposes_sysroot, check_native_dependencies_remain_shared_across_targets]
