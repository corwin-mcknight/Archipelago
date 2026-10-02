#!/usr/bin/env python3
"""Check coverage exports survive a real failing, instrumented runner."""

import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parent.parent


def main():
    spec = importlib.util.spec_from_file_location("host_coverage", ROOT / "tools/host-coverage.py")
    coverage = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(coverage)
    compiler = os.environ.get("CXX", shutil.which("clang++"))
    if not compiler:
        raise SystemExit("clang++ not found")
    with tempfile.TemporaryDirectory(prefix="archipelago-host-coverage-") as tmp:
        directory = Path(tmp)
        source = directory / "failure.cpp"
        source.write_text("int main(int argc, char**) { return argc > 0 ? 1 : 0; }\n")
        runner = directory / "runner"
        subprocess.run([compiler, "-fprofile-instr-generate", "-fcoverage-mapping", str(source),
                        "-o", str(runner)], check=True)
        coverage.OBJDIR = str(directory / "unused-objects")
        coverage.TOOLDIR = str(directory / "unused-tools")
        coverage.RUNNER = str(runner)
        coverage.COVDIR = str(directory / "coverage")
        coverage.RAWDIR = str(directory / "coverage/raw")
        actual_run = coverage.run

        def fixture_build(cmd, **kwargs):
            # The fixture replaces only Plume's build stage; execution and both LLVM tools are real.
            if cmd[1:5] == ["-m", "plume", "build", "test/kernel-testrunner"]:
                return subprocess.CompletedProcess(cmd, 0)
            return actual_run(cmd, **kwargs)

        coverage.run = fixture_build
        result = coverage.main([])
        summary = json.loads((directory / "coverage/coverage.json").read_text())
        assert summary["lines"]["count"] > 0, summary
        assert (directory / "coverage/cov.profdata").is_file()
        assert result != 0, "coverage command hid the instrumented runner's failure"
        print("host coverage: failed runner returned failure and preserved coverage artifacts")


if __name__ == "__main__":
    main()
