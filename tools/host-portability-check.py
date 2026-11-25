#!/usr/bin/env python3
"""Compile and run the real hosted registry with ASan and both crash paths."""

import json
import os
from pathlib import Path
import platform
import resource
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parent.parent
KERNEL = ROOT / "src/sys/kernel"


def main():
    compiler = os.environ.get("CXX", shutil.which("clang++"))
    if not compiler:
        raise SystemExit("clang++ not found")
    sanitizer = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    common = [compiler, "-std=c++20", "-g", "-O1", *sanitizer]
    includes = ["-I", str(KERNEL / "includes")]
    amplifier = [*common, "-ffreestanding", "-nostdinc++", "-fno-builtin",
                 "-fno-exceptions", "-fno-rtti", "-I", str(KERNEL / "includes/std"), *includes]
    with tempfile.TemporaryDirectory(prefix="archipelago-host-registry-") as tmp:
        directory = Path(tmp)
        normal_fixture = directory / "registry_normal.cpp"
        normal_fixture.write_text('''#include <kernel/testing/testing.h>
KTEST(native_registry_normal, "native") {}
''')
        fixture = directory / "registry.cpp"
        fixture.write_text(r'''#include <kernel/testing/testing.h>
#include <kernel/panic.h>
KTEST_WITH_FLAGS(native_registry_panic, "native", kernel::testing::KTEST_FLAG_EXPECTS_CRASH) {
    panic("expected registry check panic");
}
KTEST_WITH_FLAGS(native_registry_assertion, "native", kernel::testing::KTEST_FLAG_EXPECTS_CRASH) {
    // More than 1024 bytes once escaped, while comfortably within the raw diagnostic limit.
    char operand[201];
    for (char& c : operand) { c = '\x01'; }
    operand[200] = '\0';
    kernel::testing::report_assertion(false, true, "fixture", 79, "\"quoted\"\\\n\r\t", operand, "", "");
}
KTEST_WITH_FLAGS(native_registry_signal, "native", kernel::testing::KTEST_FLAG_EXPECTS_CRASH) {
    __builtin_trap();
}
''')
        objects = []
        sources = [normal_fixture, fixture, KERNEL / "core/std/stdlib.cpp"]
        for index, source in enumerate(sources):
            obj = directory / f"amplifier-{index}.o"
            subprocess.run([*amplifier, "-c", str(source), "-o", str(obj)], check=True)
            objects.append(str(obj))
        harness = directory / "harness.o"
        subprocess.run([*common, *includes, "-c", str(KERNEL / "tests/runner/host_runner.cpp"),
                        "-o", str(harness)], check=True)
        linker = [] if platform.system() == "Darwin" else [
            "-fuse-ld=lld", f"-Wl,-T,{KERNEL / 'tests/runner/ktests_brackets.ld'}"]
        runner = directory / "runner"
        subprocess.run([*common, *objects, str(harness), *linker, "-o", str(runner)], check=True)
        env = dict(os.environ, ASAN_OPTIONS="abort_on_error=1:detect_leaks=0",
                   UBSAN_OPTIONS="abort_on_error=1:print_stacktrace=1")
        result = subprocess.run([str(runner)], env=env, capture_output=True, text=True, check=True)
        events = [json.loads(line.removeprefix("@@HARNESS ")) for line in result.stdout.splitlines()
                  if line.startswith("@@HARNESS ")]
        errors = [event["message"] for event in events if event["event"] == "error"]
        assertion = '"quoted"\\\n\r\t  (= ' + '\x01' * 200 + ')  at fixture:79'
        assert assertion in errors, errors
        completed = [event for event in events if event["event"] == "test_end"]
        expected = {"native_registry_normal", "native_registry_panic", "native_registry_signal",
                    "native_registry_assertion"}
        assert {event["name"] for event in completed} == expected, result.stdout
        assert len(completed) == len(expected), result.stdout
        assert all(event["status"] == "pass" for event in completed), result.stdout
        # Child RSS cannot exceed the largest child observed by this process (including compilation).
        # Darwin's raw byte values would exceed this KiB bound, catching accidental unit changes.
        peak = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
        peak_kib = peak / 1024 if platform.system() == "Darwin" else peak
        rss_events = [event for event in events if event["event"] == "test_meta"]
        assert len(rss_events) == len(expected), result.stdout
        assert all(0 < event["peak_rss_kb"] <= peak_kib for event in rss_events), result.stdout
        print("host registry: 4/4 passed (multiple translation units, ASan, panic/signal crashes, escaped assertions, KiB RSS)")


if __name__ == "__main__":
    main()
