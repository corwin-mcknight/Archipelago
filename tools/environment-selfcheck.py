#!/usr/bin/env python3
"""Regression checks for native environment selection (no installed tools required)."""

import importlib.util
from contextlib import redirect_stdout
import io
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parent.parent


def executable(path, body):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("#!/bin/sh\n" + body + "\n")
    path.chmod(0o755)


class EnvironmentChecks(unittest.TestCase):
    def wrapper(self, system):
        with tempfile.TemporaryDirectory(prefix="archipelago environment ") as temporary:
            root = Path(temporary).resolve() / "checkout with spaces"
            (root / "tools").mkdir(parents=True)
            source = ROOT / "tools/dev"
            self.assertTrue(source.exists(), "tools/dev must implement the command environment")
            shutil.copy(source, root / "tools/dev")
            fake = Path(temporary) / "commands"
            executable(fake / "uname", f"echo {system}")
            for formula, command in (("llvm", "clang"), ("lld", "ld.lld"), ("make", "make"),
                                     ("coreutils", "sha256sum"), ("curl", "curl")):
                directory = "libexec/gnubin" if formula in ("make", "coreutils") else "bin"
                executable(root / formula / directory / command, "exit 0")
            executable(fake / "brew", 'test "$1" = --prefix || exit 1\nprintf "%s/%s\\n" "$FIXTURE_ROOT" "$2"')
            executable(root / ".venv/bin/python3", "exit 0")
            env = dict(os.environ, PATH=f"{fake}:/usr/bin:/bin", FIXTURE_ROOT=str(root))
            result = subprocess.run([str(root / "tools/dev"), "/bin/sh", "-c",
                                     'pwd; command -v python3; command -v clang; '
                                     'command -v ld.lld; command -v make; command -v sha256sum; command -v curl'],
                                    cwd=temporary, env=env, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            lines = result.stdout.splitlines()
            self.assertEqual(lines[0], os.path.realpath(temporary))
            self.assertEqual(lines[1], str(root / ".venv/bin/python3"))
            if system == "Darwin":
                self.assertEqual(lines[2:], [str(root / "llvm/bin/clang"), str(root / "lld/bin/ld.lld"),
                                            str(root / "make/libexec/gnubin/make"),
                                            str(root / "coreutils/libexec/gnubin/sha256sum"),
                                            str(root / "curl/bin/curl")])
            else:
                self.assertNotIn(str(root / "llvm"), result.stdout)

    def test_darwin_selects_brew_tools_and_checkout_python_from_other_cwd(self):
        self.wrapper("Darwin")

    def test_linux_uses_local_python_without_brew(self):
        # A restricted PATH still supplies real Linux-style command names; the fake uname
        # prevents Homebrew discovery even when this check is run on macOS.
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "tools").mkdir()
            source = ROOT / "tools/dev"
            self.assertTrue(source.exists(), "tools/dev must implement the command environment")
            shutil.copy(source, root / "tools/dev")
            executable(root / "commands/uname", "echo Linux")
            executable(root / "commands/brew", 'echo "brew should not run" >&2; exit 99')
            executable(root / ".venv/bin/python3", "exit 0")
            env = dict(os.environ, PATH=f"{root}/commands:/usr/bin:/bin")
            result = subprocess.run([str(root / "tools/dev"), "python3"], env=env, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stderr, b"")

    def doctor(self):
        path = ROOT / "tools/doctor.py"
        self.assertTrue(path.exists(), "doctor must diagnose missing and obsolete tools")
        spec = importlib.util.spec_from_file_location("native_doctor", path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def test_doctor_rejects_old_clang(self):
        doctor = self.doctor()
        with tempfile.TemporaryDirectory() as temporary:
            executable(Path(temporary) / "clang", 'echo "clang version 16.0.0"')
            with patch.dict(os.environ, PATH=temporary), redirect_stdout(io.StringIO()) as output:
                self.assertFalse(doctor.check_tool("clang", minimum=17))
            self.assertIn("LLVM 17+ required", output.getvalue())

    def test_doctor_reports_missing_tool_without_creating_files(self):
        doctor = self.doctor()
        with tempfile.TemporaryDirectory() as temporary:
            with patch.dict(os.environ, PATH=temporary), redirect_stdout(io.StringIO()) as output:
                self.assertFalse(doctor.check_tool("ld.lld"))
            self.assertIn("MISSING ld.lld", output.getvalue())
            self.assertEqual(list(Path(temporary).iterdir()), [])

    def sdk_doctor(self, sdk_kind, command_status=0):
        doctor = self.doctor()
        required = next(line.split("==", 1)[1] for line in (ROOT / "requirements.txt").read_text().splitlines()
                        if line.startswith("PyYAML=="))
        with tempfile.TemporaryDirectory(prefix="archipelago SDK ") as temporary:
            root = Path(temporary)
            sdk = root / "MacOSX.sdk"
            if sdk_kind == "directory":
                sdk.mkdir()
            elif sdk_kind == "file":
                sdk.write_text("SDK paths must be directories")
            if sdk_kind != "no-xcrun":
                executable(root / "xcrun", 'test "$*" = "--sdk macosx --show-sdk-path" || exit 99\n'
                           f'printf "%s\\n" "{sdk}"\nexit {command_status}')
            before = sorted(path.relative_to(root) for path in root.rglob("*"))
            # Other tools are outside this regression; the SDK discovery subprocess and filesystem are real.
            with patch.dict(os.environ, PATH=temporary), patch.object(doctor.platform, "system", return_value="Darwin"), \
                    patch.object(doctor, "check_tool", return_value=True), \
                    patch.object(doctor.sys, "prefix", str(ROOT / ".venv")), \
                    patch.object(doctor.importlib.metadata, "version", return_value=required), \
                    redirect_stdout(io.StringIO()) as output:
                result = doctor.main(["--arch", "x86_64"])
            self.assertEqual(sorted(path.relative_to(root) for path in root.rglob("*")), before)
            return result, output.getvalue()

    def test_doctor_rejects_missing_macos_sdk_directory(self):
        result, output = self.sdk_doctor("missing")
        self.assertEqual(result, 1, output)
        self.assertIn("SDK", output)

    def test_doctor_rejects_macos_sdk_path_that_is_a_file(self):
        result, output = self.sdk_doctor("file")
        self.assertEqual(result, 1, output)

    def test_doctor_rejects_failed_sdk_discovery_even_with_existing_path(self):
        result, output = self.sdk_doctor("directory", command_status=1)
        self.assertEqual(result, 1, output)

    def test_doctor_reports_missing_xcrun_without_creating_files(self):
        result, output = self.sdk_doctor("no-xcrun")
        self.assertEqual(result, 1, output)
        self.assertIn("xcrun", output)

    def test_doctor_accepts_existing_macos_sdk_directory(self):
        result, output = self.sdk_doctor("directory")
        self.assertEqual(result, 0, output)

    def test_doctor_checks_archive_tools_on_linux(self):
        doctor = self.doctor()
        check_tool = doctor.check_tool
        required = next(line.split("==", 1)[1] for line in (ROOT / "requirements.txt").read_text().splitlines()
                        if line.startswith("PyYAML=="))
        with tempfile.TemporaryDirectory() as temporary:
            def check_archive(name, minimum=None):
                return check_tool(name, minimum) if name in ("ar", "tar") else True
            with patch.dict(os.environ, PATH=temporary), patch.object(doctor.platform, "system", return_value="Linux"), \
                    patch.object(doctor, "check_tool", side_effect=check_archive), \
                    patch.object(doctor.sys, "prefix", str(ROOT / ".venv")), \
                    patch.object(doctor.importlib.metadata, "version", return_value=required), \
                    redirect_stdout(io.StringIO()) as output:
                result = doctor.main(["--arch", "riscv64"])
            self.assertEqual(result, 1, output.getvalue())
            self.assertIn("MISSING ar", output.getvalue())
            self.assertIn("MISSING tar", output.getvalue())
            self.assertEqual(list(Path(temporary).iterdir()), [])


if __name__ == "__main__":
    unittest.main()
