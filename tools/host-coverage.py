#!/usr/bin/env python3
"""Host-tier line-coverage gate (step 5 of the two-tier test system).

Builds the host test runner with LLVM source-based coverage instrumentation, runs the whole host
tier (each test is a forked child that flushes its own raw profile), merges the profiles, and reports
total line coverage over the kernel logic under test (test files and system headers excluded).

The gate is metric-agnostic by design: this tool always produces the coverage data and prints the
number; it only *fails* when --min (or COV_MIN) is given, so CI applies whatever policy is active
(absolute global threshold now, diff-coverage later) over the same data. See the spec's Coverage Gate.

Usage:
  host-coverage.py [--min PCT] [test_name ...]
  COV_MIN=70 host-coverage.py        # gate in CI
"""

import glob
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OBJDIR = os.path.join(ROOT, "build", "obj", "test", "kernel-testrunner")
RUNNER = os.path.join(OBJDIR, "host-test-runner")
COVDIR = os.path.join(ROOT, "build", "host-cov")
RAWDIR = os.path.join(COVDIR, "raw")
# Coverage of the code under test only: drop the test sources themselves and any system/libc headers.
IGNORE_REGEX = r"(/tests/|/usr/|/include/c\+\+/|/musl/)"


def find_tool(name):
    p = shutil.which(name)
    if p:
        return p
    for cand in sorted(glob.glob(f"/usr/lib/llvm*/bin/{name}"), reverse=True):
        return cand
    sys.exit(f"host-coverage: {name} not found (install the llvm package, e.g. apk add llvm21)")


def run(cmd, **kw):
    print("  $", " ".join(cmd))
    return subprocess.run(cmd, check=True, **kw)


def main(argv):
    min_pct = os.environ.get("COV_MIN")
    names = []
    i = 0
    while i < len(argv):
        if argv[i] == "--min":
            min_pct = argv[i + 1]
            i += 2
        else:
            names.append(argv[i])
            i += 1
    min_pct = float(min_pct) if min_pct not in (None, "") else None

    profdata = find_tool("llvm-profdata")
    cov = find_tool("llvm-cov")

    # Force a clean coverage rebuild: the normal build has no instrumentation and plume would skip it.
    shutil.rmtree(OBJDIR, ignore_errors=True)
    shutil.rmtree(COVDIR, ignore_errors=True)
    os.makedirs(RAWDIR, exist_ok=True)

    env = dict(os.environ, COVERAGE="1")
    print("[host-coverage] building instrumented runner")
    run([sys.executable, "-m", "plume", "build", "test/kernel-testrunner"], cwd=ROOT, env=env)
    if not os.path.exists(RUNNER):
        sys.exit(f"host-coverage: runner not built at {RUNNER}")

    print("[host-coverage] running host tier under instrumentation")
    # %m = LLVM's documented online-merge pool: each forked child merges its counters into the pooled
    # file, so the union of all tests' coverage accumulates (the runner is sequential, so no write race).
    run_env = dict(os.environ,
                   LLVM_PROFILE_FILE=os.path.join(RAWDIR, "%m.profraw"),
                   ASAN_OPTIONS="abort_on_error=1:detect_leaks=0")
    # The runner returns nonzero if any test fails; coverage still wants the data, so don't check=True.
    subprocess.run([RUNNER] + names, cwd=ROOT, env=run_env)

    raws = glob.glob(os.path.join(RAWDIR, "*.profraw"))
    if not raws:
        sys.exit("host-coverage: no .profraw produced -- was the runner built with COVERAGE=1?")

    merged = os.path.join(COVDIR, "cov.profdata")
    run([profdata, "merge", "-sparse"] + raws + ["-o", merged])

    out = run([cov, "export", "-summary-only", f"-instr-profile={merged}",
               f"-ignore-filename-regex={IGNORE_REGEX}", RUNNER],
              capture_output=True, text=True).stdout
    totals = json.loads(out)["data"][0]["totals"]
    lines = totals["lines"]
    pct = lines["percent"]

    summary = {"tier": "host", "lines": lines, "functions": totals.get("functions"),
               "regions": totals.get("regions"), "min_required": min_pct}
    with open(os.path.join(COVDIR, "coverage.json"), "w") as f:
        json.dump(summary, f, indent=2)

    print(f"\nhost line coverage: {pct:.2f}% ({lines['covered']}/{lines['count']} lines)")
    print(f"artifacts: {COVDIR}/ (coverage.json, cov.profdata)")
    if min_pct is not None:
        if pct < min_pct:
            print(f"FAIL: below gate of {min_pct:.2f}%")
            return 1
        print(f"OK: meets gate of {min_pct:.2f}%")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
