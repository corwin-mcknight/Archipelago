"""Transport-agnostic @@HARNESS event protocol: parser and result aggregator.

One protocol, two transports. The kernel emits @@HARNESS JSON events; the host-tier runner writes
them to stdout and the QEMU tier writes them over serial. This module turns a line stream from either
transport into one result schema, so a single aggregator serves both tiers.

Result schema (per test): {id, tier, outcome, duration_ns, failures[], diagnostics{}}.
"""

import json
from xml.sax.saxutils import escape, quoteattr

HARNESS_PREFIX = "@@HARNESS "


def parse_line(line):
    """Parse one line into an event dict, or None if it is not a @@HARNESS event."""
    s = line.strip()
    if not s.startswith(HARNESS_PREFIX):
        return None
    try:
        return json.loads(s[len(HARNESS_PREFIX):])
    except json.JSONDecodeError:
        return None


class TestResult:
    def __init__(self, name, tier):
        self.name = name
        self.tier = tier
        self.outcome = "incomplete"
        self.duration_ns = None
        self.failures = []
        self.diagnostics = {}

    def to_dict(self):
        return {
            "id": self.name,
            "tier": self.tier,
            "outcome": self.outcome,
            "duration_ns": self.duration_ns,
            "failures": self.failures,
            "diagnostics": self.diagnostics,
        }


class Aggregator:
    """Consumes @@HARNESS events (from any transport) and builds per-test results."""

    def __init__(self, tier):
        self.tier = tier
        self.results = []
        self._by_name = {}
        self._current = None

    def _get(self, name):
        r = self._by_name.get(name)
        if r is None:
            r = TestResult(name, self.tier)
            self._by_name[name] = r
            self.results.append(r)
        return r

    def feed_event(self, ev):
        kind = ev.get("event")
        if kind == "test_start":
            self._current = self._get(ev.get("name", "?"))
            self._current.outcome = "incomplete"
        elif kind == "error":
            if self._current is not None:
                self._current.failures.append(ev.get("message", ""))
        elif kind == "test_end":
            r = self._get(ev.get("name", "?"))
            r.outcome = "pass" if ev.get("status") == "pass" else "fail"
            if "duration_ns" in ev:
                r.duration_ns = ev["duration_ns"]
            reason = ev.get("reason")
            if reason and r.outcome == "fail" and reason not in r.failures:
                r.failures.append(reason)
            self._current = None
        elif kind == "test_meta":
            # Out-of-band per-test diagnostics emitted after the test (e.g. parent-measured peak RSS).
            r = self._get(ev.get("name", "?"))
            for k, v in ev.items():
                if k not in ("event", "name"):
                    r.diagnostics[k] = v

    def feed_line(self, line):
        ev = parse_line(line)
        if ev is not None:
            self.feed_event(ev)
        return ev

    def finalize(self):
        """A test_start with no matching test_end means the test crashed or hung; mark it failed."""
        for r in self.results:
            if r.outcome == "incomplete":
                r.outcome = "fail"
                r.failures.append("no test_end (test crashed or hung)")

    def counts(self):
        passed = sum(1 for r in self.results if r.outcome == "pass")
        failed = sum(1 for r in self.results if r.outcome == "fail")
        return len(self.results), passed, failed


def write_junit(result_dicts, path, suite_name):
    """Emit JUnit XML for CI from result dicts (the shared {id,tier,outcome,duration_ns,failures,...}
    schema). One <testsuite>; each result is a <testcase> with a <failure> child when it did not pass."""
    failures = sum(1 for r in result_dicts if r.get("outcome") != "pass")
    total_time = sum((r.get("duration_ns") or 0) for r in result_dicts) / 1e9
    lines = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        f'<testsuites tests="{len(result_dicts)}" failures="{failures}" time="{total_time:.6f}">',
        f'  <testsuite name={quoteattr(suite_name)} tests="{len(result_dicts)}" '
        f'failures="{failures}" time="{total_time:.6f}">',
    ]
    for r in result_dicts:
        t = (r.get("duration_ns") or 0) / 1e9
        name = r.get("id", "?")
        lines.append(f'    <testcase name={quoteattr(name)} classname={quoteattr(suite_name)} time="{t:.6f}">')
        if r.get("outcome") != "pass":
            msg = "; ".join(r.get("failures") or []) or "test failed"
            lines.append(f'      <failure message={quoteattr(msg[:200])}>{escape(msg)}</failure>')
        lines.append("    </testcase>")
    lines.append("  </testsuite>")
    lines.append("</testsuites>")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")
