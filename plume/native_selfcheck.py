"""Behavior checks for native tool selection, invalidation, and dry-run indexing."""

from contextlib import redirect_stdout
import io
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
from types import SimpleNamespace
from unittest.mock import patch

from plume.builder import build_needed, build_package
from plume.config import Config
from plume.env import get_build_env, package_obj_dir
from plume.package import Package
from plume.stamp import _tree_hashes

ROOT = Path(__file__).resolve().parent.parent


def _compiler(path, version="one", fail=False):
    path.write_text('#!/bin/sh\nif [ "$1" = --version ]; then echo "clang version ' + version + '"; exit 0; fi\n'
                    + ('exit 1\n' if fail else
                       'args="$*"\nwhile [ "$#" -gt 0 ]; do\n'
                       ' if [ "$1" = -o ]; then shift; output="$1"; fi\n shift\ndone\n'
                       f'printf "%s\\n" "{version} $args" > "$output"\n'))
    path.chmod(0o755)


def _fixture(root, *, tool=False, arch="x86_64", flags=""):
    root = Path(root).resolve()
    config_dir = root / "repo/config"
    config_dir.mkdir(parents=True, exist_ok=True)
    compiler = root / "compiler"
    if not compiler.exists():
        _compiler(compiler)
    makefile = root / "repo/packages/test/fixture/Makefile"
    makefile.parent.mkdir(parents=True, exist_ok=True)
    makefile.write_text('pkg_get_source pkg_configure:\n\t@true\n'
                        'pkg_build:\n\t$(MAKE) -C $(LIVE_SOURCES) OBJ_DIR=$(OBJ_DIR)\n'
                        'pkg_install:\n\tmkdir -p $(D)\n\tcp $(OBJ_DIR)/value.o $(D)/output\n'
                        + ('\tmkdir -p $(TOOL_INSTALL)/fixture\n'
                           '\tcp $(OBJ_DIR)/value.o $(TOOL_INSTALL)/fixture/output\n' if tool else ''))
    source = root / "src/fixture"
    source.mkdir(parents=True, exist_ok=True)
    (source / "value.cpp").write_text("int value = 1;\n")
    (source / "Makefile").write_text('all: $(OBJ_DIR)/value.o\n'
                                    '$(OBJ_DIR)/value.o: value.cpp\n\tmkdir -p $(OBJ_DIR)\n'
                                    '\t$(CXX) $(CXXFLAGS) -c $< -o $@\n')
    path = config_dir / f"{arch}.yaml"
    path.write_text(f'config:\n  arch: {arch}\n  board: pc\n  cc: {compiler}\n  cxx: {compiler}\n'
                    f'  make: {shutil.which("make")}\n  cxxflags: "{flags}"\n'
                    f'  build_dir: ./build/{arch}\n  tmp_path: ./build/{arch}/tmp\n'
                    f'  sysroot: ./build/{arch}/sysroot\n  tools_path: ./build/tools\n'
                    '  source_dir: ./src\n  repo_path: ./repo\n')
    package = Package.parse("test/fixture", {"supports_live_sources": True,
                                            "live_source_path": "fixture", "is_build_tool": tool}, arch=arch)
    _tree_hashes.cache_clear()
    return Config(path), package, compiler


def _build(config, package):
    with redirect_stdout(io.StringIO()):
        ok, _ = build_package(config, package)
    return ok


def check_same_path_compiler_upgrade_recompiles():
    with tempfile.TemporaryDirectory() as root:
        config, package, compiler = _fixture(root)
        assert _build(config, package)
        assert build_needed(config, package) is None
        previous = compiler.stat()
        _compiler(compiler, "two")
        os.utime(compiler, ns=(previous.st_atime_ns, previous.st_mtime_ns))
        assert build_needed(config, package) is not None, "same-path compiler replacement was accepted"
        assert _build(config, package)
        output = Path(get_build_env(config, package)["D"]) / "output"
        assert output.read_text().startswith("two "), "existing object bypassed the upgraded compiler"


def check_flags_recompile_and_failed_rebuild_has_no_success():
    with tempfile.TemporaryDirectory() as root:
        config, package, compiler = _fixture(root)
        assert _build(config, package)
        changed, package, _ = _fixture(root, flags="-DNEW_FLAG")
        assert _build(changed, package)
        output = Path(get_build_env(changed, package)["D"]) / "output"
        assert "-DNEW_FLAG" in output.read_text(), "changed flags reused an existing object"
        _compiler(compiler, "broken", fail=True)
        assert not _build(changed, package), "compiler failure bypassed by old object"
        _compiler(compiler)
        assert build_needed(config, package) is not None, "failed rebuild retained a usable success stamp"


def _check_target_flag_rebuild(key, value, output_name):
    """Exercise current kernel recipes, including their override += defaults."""
    with tempfile.TemporaryDirectory() as temporary, patch.dict(os.environ, ASFLAGS="", CWARNINGS=""):
        root = Path(temporary).resolve()
        config, package, compiler = _fixture(root)
        config.config["as"] = str(compiler)
        source = root / "src/fixture"
        for name in ("x86_64/platforms/pc", "boot/limine", "core", "task", "obj", "crash", "syscalls", "mm", "shell"):
            (source / name).mkdir(parents=True, exist_ok=True)
        (source / "core/value.cpp").write_text("int value = 1;\n")
        (source / "x86_64/value.s").write_text("bits 64\n")
        (source / "Makefile").write_text(f'include {ROOT}/src/sys/kernel/Makefile\n')
        recipe = root / "repo/packages/test/fixture/Makefile"
        recipe.write_text('pkg_get_source pkg_configure:\n\t@true\n'
                          'pkg_build:\n\t$(MAKE) -C $(LIVE_SOURCES) $(OBJ_DIR)/core/value.cpp.o $(OBJ_DIR)/x86_64/value.s.o\n'
                          'pkg_install:\n\tmkdir -p $(D)\n\tcp $(OBJ_DIR)/core/value.cpp.o $(D)/cxx\n'
                          '\tcp $(OBJ_DIR)/x86_64/value.s.o $(D)/asm\n')
        assert _build(config, package)
        assert build_needed(config, package) is None
        with patch.dict(os.environ, {key: value}):
            assert build_needed(config, package) is not None, f"changed inherited {key} was accepted as fresh"
            assert _build(config, package)
            output = Path(get_build_env(config, package)["D"]) / output_name
            assert value in output.read_text(), f"changed {key} did not reach actual Make recipe"
        # The same knob is also supported as a lower-case target configuration
        # setting, with the configured value taking precedence over the shell.
        config.config[key.lower()] = value + " -DCONFIGURED_FLAG"
        assert _build(config, package)
        output = Path(get_build_env(config, package)["D"]) / output_name
        assert "-DCONFIGURED_FLAG" in output.read_text(), f"configured {key} was not exported to Make"


def check_assembler_flags_trigger_actual_rebuild():
    _check_target_flag_rebuild("ASFLAGS", "-DASM_OPTION=1", "asm")


def check_warning_flags_trigger_actual_rebuild():
    _check_target_flag_rebuild("CWARNINGS", "-Werror", "cxx")


def check_source_only_changes_preserve_unaffected_objects():
    with tempfile.TemporaryDirectory() as root:
        config, package, _ = _fixture(root)
        assert _build(config, package)
        obj = Path(package_obj_dir(config, package)) / "unaffected.o"
        obj.write_text("keep me")
        source = Path(root) / "src/fixture/value.cpp"
        source.write_text("int value = 2;\n")
        _tree_hashes.cache_clear()
        assert _build(config, package)
        assert obj.read_text() == "keep me", "source-only change discarded unrelated objects"


def check_shared_host_tool_stamp_and_coverage_mode():
    with tempfile.TemporaryDirectory() as root:
        config, package, _ = _fixture(root, tool=True)
        with patch.dict(os.environ, COVERAGE="1"):
            assert _build(config, package)
        assert build_needed(config, package) is not None, "instrumented host tool accepted as normal tool"
        assert _build(config, package)
        other, other_package, _ = _fixture(root, tool=True, arch="riscv64")
        assert build_needed(other, other_package) is None, "identical native host tool rebuilt for another target"


def check_missing_installed_host_tool_is_not_fresh():
    with tempfile.TemporaryDirectory() as root:
        config, package, _ = _fixture(root, tool=True)
        assert _build(config, package)
        env = get_build_env(config, package)
        shutil.rmtree(env["D"])
        (Path(env["TOOL_INSTALL"]) / "fixture/output").unlink()
        assert build_needed(config, package) is not None, "stamp alone was accepted as installed output"


def check_nested_target_make_honors_selected_tools():
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary).resolve()
        config, package, compiler = _fixture(root)
        source = root / "src/fixture"
        for name in ("x86_64/platforms/pc", "boot/limine", "core", "task", "obj", "crash", "syscalls", "mm", "shell"):
            (source / name).mkdir(parents=True, exist_ok=True)
        (source / "core/value.cpp").write_text("int value = 1;\n")
        (source / "Makefile").write_text(f'include {ROOT}/src/sys/kernel/Makefile\n')
        env = get_build_env(config, package)
        env["LIVE_SOURCES"] = str(source)
        # Execute the real package Makefile/submake boundary but ask for one object only.
        wrapper = root / "outer.mk"
        wrapper.write_text('all:\n\t$(MAKE) -C $(LIVE_SOURCES) $(OBJ_DIR)/core/value.cpp.o\n')
        result = subprocess.run([env["MAKE"], "-f", str(wrapper)], env=env, capture_output=True, text=True)
        assert result.returncode == 0, result.stdout + result.stderr
        obj = Path(env["OBJ_DIR"]) / "core/value.cpp.o"
        assert obj.read_bytes().startswith(b"one "), "nested kernel Make ignored configured CXX"
        assert "-nostdinc++" in obj.read_text(), "freestanding compilation admitted host C++ headers"


def check_limine_rebuild_honors_configured_host_compiler():
    with tempfile.TemporaryDirectory() as temporary:
        config, package, _ = _fixture(temporary)
        env = get_build_env(config, package)
        source = Path(env["S"])
        source.mkdir(parents=True)
        (source / "limine.c").write_text("int main(void) { return 0; }\n")
        (source / "Makefile").write_text('CC=cc\nall: limine\nlimine: limine.c\n\t$(CC) $< -o $@\n')
        (source / "limine").write_text("obsolete executable")
        os.utime(source / "limine", (2000000000, 2000000000))
        result = subprocess.run([env["MAKE"], "-f", str(ROOT / "repo/packages/boot/limine-tools/Makefile"),
                                 "pkg_build"], env=env, cwd=source, capture_output=True, text=True)
        assert result.returncode == 0, result.stdout + result.stderr
        assert (source / "limine").read_bytes().startswith(b"one "), "Limine ignored selected CC or reused old host binary"


def check_init_private_linker_script_survives_user_flags():
    for arch, machine in (("x86_64", "1"), ("riscv64", "2")):
        with tempfile.TemporaryDirectory() as temporary:
            config, package, _ = _fixture(temporary, arch=arch)
            env = get_build_env(config, package)
            libraries = Path(env["SYSROOT"]) / "usr/lib"
            libraries.mkdir(parents=True)
            for name in ("user.mk", "arch.mk", "user.ld"):
                shutil.copy(ROOT / "src/lib/crt" / name, libraries / name)
            for name in ("libcrt.a", "libelf.a", "libinitrd.a"):
                (libraries / name).touch()
            source = Path(env["LIVE_SOURCES"])
            shutil.copy(ROOT / "src/sys/init/init.ld", source / "init.ld")
            (source / "Makefile").write_text(f'include {ROOT}/src/sys/init/Makefile\n')
            env["LDFLAGS"] = "-z defs"
            result = subprocess.run([env["MAKE"], "-n", "-B", "--no-print-directory"], cwd=source, env=env,
                                    capture_output=True, text=True)
            assert result.returncode == 0, result.stdout + result.stderr
            command = next(shlex.split(line) for line in result.stdout.splitlines() if line.startswith(env["LD"] + " "))
            assert command[command.index("-T") + 1] == "init.ld", "coordinator linked with ordinary ELF script"
            assert f"--defsym=INIT_MACHINE={machine}" in command, command
            assert "defs" in command, "caller linker flags were discarded"


def check_compilation_database_is_dry_and_checkout_specific():
    from plume.cli import cmd_clangd

    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary).resolve()
        config, _, compiler = _fixture(root)
        source = root / "src/sys/kernel"
        for name in ("riscv64/platforms/virt", "riscv64/platforms/jh7110", "boot/limine", "core", "task", "obj",
                     "crash", "syscalls", "mm", "shell"):
            (source / name).mkdir(parents=True, exist_ok=True)
        for name in ("core/common.cpp", "riscv64/platforms/virt/virt.cpp", "riscv64/platforms/jh7110/board.cpp"):
            (source / name).write_text("int value = 1;\n")
        (source / "Makefile").write_text(f'include {ROOT}/src/sys/kernel/Makefile\n')
        (root / "repo/packages/sys/kernel").mkdir(parents=True)
        (root / "repo/packages/sys/kernel/Makefile").write_text("all:\n\t@true\n")
        (root / "repo/packages.yml").write_text('sys/kernel:\n  description: fixture\n  supports_live_sources: true\n'
                                                 '  live_source_path: sys/kernel\n  varies_by: [board]\n')
        cfg = config.config_path
        Path(cfg).write_text(Path(cfg).read_text().replace("arch: x86_64", "arch: riscv64")
                            .replace("board: pc", "board: jh7110") + f"  ld: {compiler}\n")
        config = Config(cfg)
        obj = Path(config.get("build_dir")) / "obj/sys/kernel^virt"
        obj.mkdir(parents=True)
        (obj / "stale.json").write_text(json.dumps({"directory": str(source), "file": "riscv64/platforms/virt/virt.cpp",
                                                   "arguments": [str(compiler), "-c", "riscv64/platforms/virt/virt.cpp"]}))
        before = {p.relative_to(root) for p in root.rglob("*") if p.is_file()}
        with redirect_stdout(io.StringIO()):
            assert cmd_clangd(SimpleNamespace(config=cfg, arch=None)) == 0
        entries = json.loads((root / "build/compile_commands.json").read_text())
        assert {Path(e["file"]).name for e in entries} == {"common.cpp", "board.cpp"}, entries
        assert all(e["directory"] == str(source) for e in entries), entries
        assert all(str(root) in e["output"] for e in entries), entries
        after = {p.relative_to(root) for p in root.rglob("*") if p.is_file()}
        assert after - before == {Path("build/compile_commands.json")}, "indexing compiled or linked artifacts"


def check_debug_dry_run_uses_loopback_and_graphical_display():
    from plume.cli import cmd_run

    with tempfile.TemporaryDirectory() as root:
        config, _, _ = _fixture(root)
        iso = Path(root) / "image.iso"
        iso.touch()
        config.config["image_output"] = str(iso)
        tools = Path(root) / "tools"
        tools.mkdir()
        for name in ("test-harness.py", "harness_protocol.py"):
            shutil.copy(ROOT / "tools" / name, tools / name)
        arguments = SimpleNamespace(memory=None, no_display=False, debug=True, dry_run=True)
        with patch("plume.cli._load", return_value=(config, [])), redirect_stdout(io.StringIO()) as output:
            assert cmd_run(arguments) == 0
        command = output.getvalue()
        assert "-gdb tcp:127.0.0.1:1234 -S" in command, command
        assert "-display none" not in command, "graphical debug default became headless"


def _runtime_fixture(temporary):
    config, _, _ = _fixture(temporary, arch="riscv64")
    root = Path(config.project_root)
    config.config.update(image_output=str(root / "image.iso"),
                         firmware=str(root / "build/tools/edk2-riscv/RISCV_VIRT_CODE.fd"),
                         qemu=str(root / "fake-qemu"))
    executable = root / "fake-qemu"
    executable.write_text('#!/bin/sh\ncase "$*" in *-kernel*) echo "boot: initialization complete";; esac\n')
    executable.chmod(0o755)
    packages = []
    for name, output in (("sys/base", 'mkdir -p $(D)/boot; echo system > $(D)/boot/kernel'),
                         ("boot/edk2-riscv", 'mkdir -p $(TOOL_INSTALL)/edk2-riscv; '
                          'echo code > $(TOOL_INSTALL)/edk2-riscv/RISCV_VIRT_CODE.fd; '
                          'echo vars > $(TOOL_INSTALL)/edk2-riscv/RISCV_VIRT_VARS.fd'),
                         ("boot/u-boot-qemu", 'mkdir -p $(TOOL_INSTALL)/u-boot-qemu; '
                          'echo uboot > $(TOOL_INSTALL)/u-boot-qemu/u-boot.bin')):
        recipe = root / "repo/packages" / name / "Makefile"
        recipe.parent.mkdir(parents=True)
        recipe.write_text('pkg_get_source pkg_configure pkg_build:\n\t@true\n'
                          'pkg_install:\n\t' + output + '\n')
        packages.append(Package.parse(name, {"is_build_tool": name.startswith("boot/")}, arch="riscv64"))
    # Deliberately unsupported by this fixture: selecting every host tool would
    # fail, rather than hide unwanted hosted-lane work behind a fake install.
    packages.append(Package.parse("test/kernel-testrunner", {"is_build_tool": True}, arch="riscv64"))
    tools = root / "tools"
    tools.mkdir()
    (tools / "test-harness.py").write_text(
        'from pathlib import Path\nimport sys\n'
        'def machine_args(arch, iso, firmware=None, exit_device=False):\n'
        '    assert Path(firmware).is_file(), "firmware missing before launch"\n'
        '    return []\n'
        'if __name__ == "__main__":\n'
        '    assert Path(sys.argv[sys.argv.index("--firmware") + 1]).is_file(), "firmware missing before test"\n')
    return config, packages


def check_fresh_riscv_test_builds_only_required_runtime_tools():
    from plume.cli import cmd_test

    with tempfile.TemporaryDirectory() as temporary:
        config, packages = _runtime_fixture(temporary)
        def image(cfg, **kwargs):
            assert Path(cfg.get("sysroot"), "boot/kernel").is_file()
            Path(cfg.get("image_output")).touch()
            return True
        args = SimpleNamespace(tests=[], verbose=False)
        with patch("plume.cli._load", return_value=(config, packages)), patch("plume.cli.assemble_image", image), \
                redirect_stdout(io.StringIO()):
            assert cmd_test(args) == 0, "fresh RISC-V test did not prepare firmware"
        assert Path(config.get("firmware")).is_file()
        assert not Path(config.get("tools_path"), "u-boot-qemu").exists(), "ISO test built unused U-Boot"
        assert not Path(config.get("tools_path"), "kernel-testrunner").exists(), "target test built hosted test lane"


def check_fresh_riscv_run_prepares_firmware_without_system_rebuild():
    from plume.cli import cmd_run

    with tempfile.TemporaryDirectory() as temporary:
        config, packages = _runtime_fixture(temporary)
        Path(config.get("image_output")).touch()
        args = SimpleNamespace(memory=None, no_display=True, debug=False, dry_run=False)
        with patch("plume.cli._load", return_value=(config, packages)), redirect_stdout(io.StringIO()):
            assert cmd_run(args) == 0, "fresh RISC-V run did not prepare firmware"
        assert Path(config.get("firmware")).is_file()
        assert not Path(config.get("sysroot")).exists(), "run rebuilt preassembled system"
        assert not Path(config.get("tools_path"), "u-boot-qemu").exists()


def check_fresh_uboot_test_prepares_only_uboot_runtime():
    from plume.cli import cmd_uboot_test

    with tempfile.TemporaryDirectory() as temporary:
        config, packages = _runtime_fixture(temporary)
        def image(cfg, output, **kwargs):
            Path(output).parent.mkdir(parents=True, exist_ok=True)
            Path(output).touch()
            return True
        with patch("plume.cli._load", return_value=(config, packages)), patch("plume.cli.assemble_sd", image), \
                redirect_stdout(io.StringIO()):
            assert cmd_uboot_test(SimpleNamespace(verbose=False)) == 0, "fresh boot-chain test did not build U-Boot"
        assert Path(config.get("tools_path"), "u-boot-qemu/u-boot.bin").is_file()
        assert not Path(config.get("firmware")).exists(), "U-Boot boot chain built unused EDK2"
        assert not Path(config.get("sysroot")).exists(), "runtime helper rebuilt system graph"


def check_riscv_run_dry_run_never_fetches_missing_firmware():
    from plume.cli import cmd_run

    with tempfile.TemporaryDirectory() as temporary:
        config, packages = _runtime_fixture(temporary)
        Path(config.get("image_output")).touch()
        args = SimpleNamespace(memory=None, no_display=True, debug=False, dry_run=True)
        with patch("plume.cli._load", return_value=(config, packages)), redirect_stdout(io.StringIO()), \
                patch("sys.stderr", io.StringIO()) as errors:
            cmd_run(args)
        assert not Path(config.get("tools_path")).exists(), "dry run built or fetched runtime dependencies"
        assert "plume build boot/edk2-riscv" in errors.getvalue(), "missing firmware suggested a system-only build"
        assert config.config_path in errors.getvalue(), "firmware remedy lost selected configuration"


def check_missing_firmware_member_reinstalls_cached_runtime():
    from plume.cli import _ensure_runtime_tools

    with tempfile.TemporaryDirectory() as temporary:
        config, packages = _runtime_fixture(temporary)
        with redirect_stdout(io.StringIO()):
            assert _ensure_runtime_tools(config, packages) == 0
            for name in ("RISCV_VIRT_CODE.fd", "RISCV_VIRT_VARS.fd"):
                member = Path(config.get("tools_path")) / "edk2-riscv" / name
                member.unlink()
                assert _ensure_runtime_tools(config, packages) == 0
                assert member.is_file(), "runtime stamp concealed a missing firmware member"


def check_external_firmware_does_not_build_repository_provider():
    from plume.cli import _ensure_runtime_tools

    with tempfile.TemporaryDirectory() as temporary:
        config, packages = _runtime_fixture(temporary)
        config.config["firmware"] = str(Path(temporary) / "external-code.fd")
        Path(config.get("firmware")).write_text("user-provided firmware")
        with redirect_stdout(io.StringIO()):
            assert _ensure_runtime_tools(config, packages) == 0
        assert not Path(config.get("tools_path")).exists(), "custom firmware fetched unused repository provider"


def check_qemu_controls_reach_harness_without_changing_build_jobs():
    """Launch the real harness parser/worker path, replacing only QEMU I/O."""
    from plume.cli import main

    with tempfile.TemporaryDirectory() as temporary:
        config, package, _ = _fixture(temporary)
        root = Path(temporary)
        config.config["image_output"] = str(root / "image.iso")
        Path(config.get("image_output")).touch()
        tools = root / "tools"
        tools.mkdir()
        report = root / "harness-controls.json"
        (tools / "test-harness.py").write_text(
            'import json, runpy, sys\nfrom pathlib import Path\n'
            f'h = runpy.run_path({str(ROOT / "tools/test-harness.py")!r})\n'
            'g = h["main"].__globals__\nobserved = {}\n'
            'class FakeQemu(h["KernelHarness"]):\n'
            '    def start(self): observed["boot_timeout"] = self.boot_timeout\n'
            '    def restart(self): self.start()\n'
            '    def stop(self): pass\n'
            '    def list_tests(self, timeout):\n'
            '        observed["command_timeout"] = timeout\n'
            '        return [h["TestDescriptor"]("first"), h["TestDescriptor"]("second")]\n'
            '    def run_test(self, name, timeout, **kwargs):\n'
            '        observed["test_timeout"] = timeout\n'
            '        self.calls = getattr(self, "calls", 0) + 1\n'
            '        outcome = "infra" if self.calls == 1 else "pass"\n'
            '        return h["TestResult"](name, outcome, attempts=[h["AttemptLog"](self.calls, outcome, None)])\n'
            'g["KernelHarness"] = FakeQemu\n'
            'original = h["run_sharded"]\n'
            'def workers(tests, descriptors, args, artifacts, jobs):\n'
            '    observed.update(jobs=jobs, retries=args.retries)\n'
            '    return original(tests, descriptors, args, artifacts, jobs)\n'
            'g["run_sharded"] = workers\ng["os"].cpu_count = lambda: 2\n'
            'status = h["main"](sys.argv[1:])\n'
            f'Path({str(report)!r}).write_text(json.dumps(observed))\n'
            'raise SystemExit(status)\n')
        controls = ["--qemu-jobs", "2", "--qemu-boot-timeout", "120", "--qemu-command-timeout", "15",
                    "--qemu-test-timeout", "30", "--qemu-retries", "1"]
        cases = [(controls + ["--jobs", "3"], 3,
                  dict(jobs=2, boot_timeout=120, command_timeout=15, test_timeout=30, retries=1)),
                 ([], 1, dict(jobs=2, boot_timeout=30, command_timeout=5, test_timeout=5, retries=3))]
        for options, package_jobs, expected in cases:
            with patch("plume.cli._load", return_value=(config, [package])), \
                    patch("plume.cli.orchestrate", return_value=0) as build, \
                    patch("plume.cli.assemble_image", return_value=True), redirect_stdout(io.StringIO()):
                assert main(["test", *options]) == 0, "harness worker did not retry its simulated infrastructure failure"
            assert build.call_args.kwargs["jobs"] == package_jobs, "QEMU workers changed package build concurrency"
            assert json.loads(report.read_text()) == expected, "harness lost controls or changed omitted defaults"


CHECKS = [check_same_path_compiler_upgrade_recompiles,
          check_flags_recompile_and_failed_rebuild_has_no_success,
          check_assembler_flags_trigger_actual_rebuild,
          check_warning_flags_trigger_actual_rebuild,
          check_source_only_changes_preserve_unaffected_objects,
          check_shared_host_tool_stamp_and_coverage_mode,
          check_missing_installed_host_tool_is_not_fresh,
          check_nested_target_make_honors_selected_tools,
          check_limine_rebuild_honors_configured_host_compiler,
          check_init_private_linker_script_survives_user_flags,
          check_compilation_database_is_dry_and_checkout_specific,
          check_debug_dry_run_uses_loopback_and_graphical_display,
          check_fresh_riscv_test_builds_only_required_runtime_tools,
          check_fresh_riscv_run_prepares_firmware_without_system_rebuild,
          check_fresh_uboot_test_prepares_only_uboot_runtime,
          check_riscv_run_dry_run_never_fetches_missing_firmware,
          check_missing_firmware_member_reinstalls_cached_runtime,
          check_external_firmware_does_not_build_repository_provider,
          check_qemu_controls_reach_harness_without_changing_build_jobs]


if __name__ == "__main__":
    failed = 0
    for check in CHECKS:
        try:
            check()
            print(f"ok {check.__name__}")
        except Exception as error:
            failed += 1
            print(f"FAIL {check.__name__}: {error}")
    raise SystemExit(bool(failed))
