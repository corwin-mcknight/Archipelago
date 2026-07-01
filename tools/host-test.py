#!/usr/bin/env python3
"""Host transport for the shared harness protocol.

Launches the fork-per-test host runner, streams its merged stdout/stderr through the tier-agnostic
Aggregator, mirrors everything to console.log, and writes events.jsonl / harness.json. Sanitizer
failures are configured to abort (so the runner can attribute them to the crashing test).

Usage: host-test.py [test_name ...]   # no names => run all host-tier tests
"""

import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness_protocol import Aggregator, parse_line, write_junit

RUNNER = os.path.join("build", "tools", "kernel-testrunner", "host-test-runner")
ARTIFACTS = os.path.join("build", "host-test-artifacts")


def main(argv):
    if not os.path.exists(RUNNER):
        print(f"host-test: runner not found at {RUNNER}", file=sys.stderr)
        print("  build it first: python3 -m plume build test/kernel-testrunner", file=sys.stderr)
        return 2

    os.makedirs(ARTIFACTS, exist_ok=True)

    env = dict(os.environ)
    # Make sanitizer failures abort (a signal) rather than _exit, so the parent runner attributes them
    # to the crashing test instead of leaving a dangling result.
    env.setdefault("ASAN_OPTIONS", "abort_on_error=1:detect_leaks=0")
    env.setdefault("UBSAN_OPTIONS", "abort_on_error=1:print_stacktrace=1")

    agg = Aggregator("host")
    console_path = os.path.join(ARTIFACTS, "console.log")
    events_path = os.path.join(ARTIFACTS, "events.jsonl")

    proc = subprocess.Popen(
        [os.path.abspath(RUNNER)] + list(argv),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        env=env,
    )

    with open(console_path, "w") as console, open(events_path, "w") as events:
        for line in proc.stdout:
            console.write(line)
            ev = parse_line(line)
            if ev is not None:
                events.write(json.dumps(ev) + "\n")
                agg.feed_event(ev)
            else:
                # Non-harness output (sanitizer reports, the runner's summary) is echoed live.
                sys.stdout.write(line)
    proc.wait()

    agg.finalize()
    total, passed, failed = agg.counts()

    harness = {
        "tier": "host",
        "total": total,
        "passed": passed,
        "failed": failed,
        "runner_returncode": proc.returncode,
        "results": [r.to_dict() for r in agg.results],
    }
    with open(os.path.join(ARTIFACTS, "harness.json"), "w") as f:
        json.dump(harness, f, indent=2)
    write_junit(harness["results"], os.path.join(ARTIFACTS, "junit.xml"), "host")

    for r in agg.results:
        if r.outcome != "pass":
            print(f"  FAIL {r.name}")
            for msg in r.failures:
                print(f"        {msg}")

    print(f"\nhost tier: {passed}/{total} passed, {failed} failed")
    print(f"artifacts: {ARTIFACTS}/")
    # The runner crashing (nonzero rc with no failures parsed) is itself a failure.
    return 0 if (failed == 0 and proc.returncode == 0) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
