#!/usr/bin/env python3
"""Append CI evidence to GITHUB_STEP_SUMMARY without replacing any test command.

Run after artifact uploads with CI_STEPS=${{ toJSON(steps) }} and
CI_JOB_STATUS=${{ job.status }}. Missing reports are normal after a setup/build
failure: report the missing evidence while retaining the original step outcomes.
"""

import argparse
import html
import json
import os
from pathlib import Path
import sys


SCOPES = {
    "host": "Hosted kernel tests with ASan and UBSan.",
    "coverage": "Hosted kernel line coverage; the test runner and coverage gate must both pass.",
    "qemu": "Freestanding kernel tests under QEMU emulation.",
    "board": "Build-only: JH7110 kernel and SD image generation.",
    "sanitizers": "ThreadSanitizer stress tests and bounded parser/data-structure fuzzing.",
}


def text(value):
    """Keep report content on one Markdown line, without interpreting its markup."""
    value = html.escape(str(value)).replace("\n", " ").replace("\r", " ")
    for character in "\\`*_[]|":
        value = value.replace(character, "\\" + character)
    return value


def load_report(path):
    report = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(report, dict):
        raise ValueError("expected a JSON object")
    return report


def test_summary(path):
    try:
        report = load_report(path)
        results = report["results"]
        if not isinstance(results, list) or any(
            not isinstance(result, dict)
            or not isinstance(result.get("id"), str)
            or result.get("outcome") not in ("pass", "fail", "incomplete")
            or not isinstance(result.get("diagnostics", {}), dict)
            or not isinstance(result.get("failures", []), list)
            for result in results
        ):
            raise ValueError("invalid results array")
    except (OSError, UnicodeError, ValueError, KeyError, TypeError) as error:
        return [f"**Test counts unavailable:** {text(path)} ({text(error)}).", ""]

    # QEMU's shared schema counts skips as passes. Preserve the richer outcome.
    skipped = sum(result.get("diagnostics", {}).get("detailed_outcome") == "skipped"
                  for result in results)
    failed = [result for result in results if result["outcome"] != "pass"]
    passed = len(results) - skipped - len(failed)
    lines = ["| Total | Passed | Failed / incomplete | Skipped |",
             "| ---: | ---: | ---: | ---: |",
             f"| {len(results)} | {passed} | {len(failed)} | {skipped} |", ""]
    if "runner_returncode" in report:
        lines.extend([f"Runner exit status: {text(report['runner_returncode'])}.", ""])
    if failed:
        lines.extend(["**Failures** (up to 20; full diagnostics are in the artifacts):", ""])
        for result in failed[:20]:
            reason = "; ".join(str(value) for value in result.get("failures", []))
            lines.append(f"- **{text(result['id'])}**: {text((reason or result['outcome'])[:1000])}")
        lines.append("")
    return lines


def coverage_summary(path):
    try:
        report = load_report(path)
        coverage = report["lines"]
        percent = float(coverage["percent"])
        covered, total = int(coverage["covered"]), int(coverage["count"])
        minimum = report.get("min_required")
        lines = [f"**Line coverage: {percent:.2f}% ({covered}/{total} lines).**", ""]
        if minimum is not None:
            minimum = float(minimum)
            outcome = "met" if percent >= minimum else "not met"
            lines.extend([f"Line coverage gate: {minimum:.2f}% — {outcome}.", ""])
        return lines
    except (OSError, UnicodeError, ValueError, KeyError, TypeError) as error:
        return [f"**Coverage unavailable:** {text(path)} ({text(error)}).", ""]


def step_summary(steps):
    lines = []
    if steps:
        lines.extend(["| Step | Outcome |", "| --- | --- |"])
        for name, step in steps.items():
            lines.append(f"| {text(name.replace('_', ' '))} | {text(step.get('outcome', 'unknown'))} |")
        lines.append("")
    artifacts = [(name, step.get("outputs", {}).get("artifact-url"))
                 for name, step in steps.items() if name.endswith("_artifact")]
    if artifacts:
        lines.extend(["**Artifacts** (retained for 14 days):", ""])
        for name, url in artifacts:
            label = text(name.replace("_", " "))
            if url:
                lines.append(f"- [{label}]({url})")
            else:
                lines.append(f"- {label}: not uploaded; inspect the upload step and logs.")
        lines.append("")
    return lines


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--title", required=True)
    parser.add_argument("--kind", choices=SCOPES, required=True)
    parser.add_argument("--results", type=Path)
    parser.add_argument("--coverage", type=Path)
    args = parser.parse_args()

    lines = [f"## {text(args.title)}", "",
             f"**Job status: {text(os.environ.get('CI_JOB_STATUS', 'unknown'))}**", "",
             SCOPES[args.kind], "", "**Hardware validation: not run in this job.**", ""]
    if args.results:
        lines.extend(test_summary(args.results))
    if args.coverage:
        lines.extend(coverage_summary(args.coverage))
    steps = json.loads(os.environ.get("CI_STEPS", "{}"))
    lines.extend(step_summary(steps))
    server = os.environ.get("GITHUB_SERVER_URL", "https://github.com")
    repo, run = os.environ.get("GITHUB_REPOSITORY"), os.environ.get("GITHUB_RUN_ID")
    if repo and run:
        lines.extend([f"[Workflow run and full logs]({server}/{repo}/actions/runs/{run})", ""])
    summary = "\n".join(lines) + "\n"
    destination = os.environ.get("GITHUB_STEP_SUMMARY")
    if destination:
        with open(destination, "a", encoding="utf-8") as output:
            output.write(summary)
    else:
        sys.stdout.write(summary)


if __name__ == "__main__":
    main()
