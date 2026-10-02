#!/usr/bin/env python3
"""Exercise CI summaries with representative harness and Actions inputs."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).with_name("ci-summary.py")


class SummaryChecks(unittest.TestCase):
    def summarize(self, kind, *, report=None, coverage=None, status="success", steps=None):
        with tempfile.TemporaryDirectory(prefix="archipelago-ci-summary-") as tmp:
            directory = Path(tmp)
            output = directory / "summary.md"
            output.write_text("Existing summary\n", encoding="utf-8")
            args = [sys.executable, str(SCRIPT), "--title", "Fixture job", "--kind", kind]
            for flag, value in (("--results", report), ("--coverage", coverage)):
                if value is not None:
                    path = directory / (flag.removeprefix("--") + ".json")
                    if value != "missing":
                        path.write_text(value if isinstance(value, str) else json.dumps(value),
                                        encoding="utf-8")
                    args.extend([flag, str(path)])
            env = dict(os.environ, GITHUB_STEP_SUMMARY=str(output), CI_JOB_STATUS=status,
                       CI_STEPS=json.dumps(steps or {}), GITHUB_SERVER_URL="https://github.com",
                       GITHUB_REPOSITORY="example/project", GITHUB_RUN_ID="42")
            result = subprocess.run(args, env=env, capture_output=True, text=True)
            summary = output.read_text(encoding="utf-8")
            self.assertIn("Fixture job", summary, result.stderr)
            self.assertTrue(summary.startswith("Existing summary\n"))
            return result, summary

    def test_host_failure_and_runner_exit_are_visible(self):
        _, summary = self.summarize("host", status="failure", report={
            "tier": "host", "total": 2, "passed": 1, "failed": 1, "runner_returncode": 1,
            "results": [
                {"id": "ok", "tier": "host", "outcome": "pass", "duration_ns": 100,
                 "failures": [], "diagnostics": {}},
                {"id": "broken|test", "tier": "host", "outcome": "fail", "duration_ns": None,
                 "failures": ["expected <value>\nnext line"], "diagnostics": {}},
            ],
        })
        self.assertIn("| 2 | 1 | 1 | 0 |", summary)
        self.assertIn("**Job status: failure**", summary)
        self.assertIn("Runner exit status: 1", summary)
        self.assertIn("broken\\|test", summary)
        self.assertIn("expected &lt;value&gt;", summary)

    def test_qemu_skips_are_not_reported_as_passes(self):
        _, summary = self.summarize("qemu", report={
            "tier": "qemu", "total": 3, "passed": 2, "failed": 1,
            "results": [
                {"id": "ok", "outcome": "pass", "failures": [], "diagnostics": {}},
                {"id": "unsupported", "outcome": "pass", "failures": [],
                 "diagnostics": {"detailed_outcome": "skipped"}},
                {"id": "hung", "outcome": "fail", "failures": ["boot timed out"],
                 "diagnostics": {"detailed_outcome": "timeout", "attempts": 2}},
            ],
        })
        self.assertIn("| 3 | 1 | 1 | 1 |", summary)
        self.assertIn("boot timed out", summary)
        self.assertIn("Hardware validation: not run", summary)

    def test_coverage_gate_does_not_hide_failed_test_step(self):
        _, summary = self.summarize("coverage", status="failure", coverage={
            "tier": "host", "lines": {"covered": 90, "count": 100, "percent": 90.0},
            "functions": None, "regions": None, "min_required": 85.0,
        }, steps={"coverage": {"outcome": "failure", "conclusion": "failure", "outputs": {}}})
        self.assertIn("90.00% (90/100 lines)", summary)
        self.assertIn("85.00%", summary)
        self.assertIn("**Job status: failure**", summary)
        self.assertIn("| coverage | failure |", summary)

    def test_missing_and_malformed_results_are_explicit(self):
        for report in ("missing", "{truncated", {"results": "bad schema"}, {"results": [{}]}):
            with self.subTest(report=report):
                _, summary = self.summarize("host", report=report, status="failure")
                self.assertIn("Test counts unavailable", summary)
                self.assertNotIn("| 0 | 0 | 0 | 0 |", summary)
                self.assertIn("**Job status: failure**", summary)

    def test_missing_coverage_is_explicit(self):
        _, summary = self.summarize("coverage", coverage="missing", status="failure")
        self.assertIn("Coverage unavailable", summary)

    def test_artifact_links_and_skipped_steps(self):
        result, summary = self.summarize("sanitizers", steps={
            "tsan": {"outcome": "failure", "conclusion": "failure", "outputs": {}},
            "fuzz": {"outcome": "skipped", "conclusion": "skipped", "outputs": {}},
            "results_artifact": {"outcome": "success", "conclusion": "success", "outputs": {
                "artifact-url": "https://github.com/example/project/actions/runs/42/artifacts/99"}},
            "image_artifact": {"outcome": "success", "conclusion": "success", "outputs": {}},
        }, status="failure")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("actions/runs/42/artifacts/99", summary)
        self.assertIn("| fuzz | skipped |", summary)
        self.assertIn("image artifact: not uploaded", summary)

    def test_board_success_is_build_only(self):
        result, summary = self.summarize("board")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Build-only", summary)
        self.assertIn("Hardware validation: not run", summary)
        self.assertNotIn("Tests passed", summary)


if __name__ == "__main__":
    unittest.main()
