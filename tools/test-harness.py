#!/usr/bin/env python3
"""Kernel testing harness that drives the Archipelago test mode over QEMU serial."""

from __future__ import annotations

import argparse
import json
import queue
import subprocess
import sys
import threading
import time
from collections import deque
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Sequence


@dataclass
class Event:
    kind: str  # 'harness', 'test', or 'log'
    payload: Optional[Dict[str, Any]]
    raw: str


@dataclass
class AttemptLog:
    attempt: int
    outcome: str  # pass, fail, error, timeout, infra
    reason: Optional[str]
    requires_clean_env: bool = False
    events: List[Event] = field(default_factory=list)
    lines: List[str] = field(default_factory=list)


@dataclass
class TestResult:
    name: str
    outcome: str
    requires_clean_env: bool = False
    attempts: List[AttemptLog] = field(default_factory=list)

    @property
    def attempts_count(self) -> int:
        return len(self.attempts)

    @property
    def final_attempt(self) -> AttemptLog:
        return self.attempts[-1]


@dataclass
class TestDescriptor:
    name: str
    module: Optional[str] = None
    requires_clean_env: bool = False
    metadata: Dict[str, Any] = field(default_factory=dict)


class HarnessError(Exception):
    pass


class HarnessTimeout(HarnessError):
    pass


class HarnessProcessExit(HarnessError):
    def __init__(self, message: str, exit_code: Optional[int] = None) -> None:
        super().__init__(message)
        self.exit_code = exit_code


class HarnessGuestExit(HarnessProcessExit):
    def __init__(self, exit_code: Optional[int], events: List[Event], lines: List[str]) -> None:
        super().__init__(f"kernel exited with status {exit_code}", exit_code)
        self.events = events
        self.lines = lines


class KernelHarness:
    def __init__(
        self,
        qemu: str,
        iso: Path,
        memory: int,
        boot_timeout: float,
        extra_args: Optional[Sequence[str]] = None,
        verbose: bool = False,
        add_exit_device: bool = True,
    ) -> None:
        self.qemu = qemu
        self.iso = iso
        self.memory = memory
        self.boot_timeout = boot_timeout
        self.extra_args = list(extra_args or [])
        self.verbose = verbose
        self.add_exit_device = add_exit_device

        self.proc: Optional[subprocess.Popen[str]] = None
        self._reader_thread: Optional[threading.Thread] = None
        self._lines: Optional[queue.Queue[Optional[str]]] = None
        self._ready = False
        self._exit_code: Optional[int] = None

    # ------------------------------------------------------------------
    # Process lifecycle helpers
    # ------------------------------------------------------------------
    def start(self) -> None:
        if self.proc and self.proc.poll() is None:
            return
        args = [
            self.qemu,
            "--cdrom",
            str(self.iso),
            "-serial",
            "stdio",
            "-display",
            "none",
            "-no-reboot",
            "-m",
            str(self.memory),
        ]
        if self.add_exit_device:
            args.extend(["-device", "isa-debug-exit,iobase=0x604,iosize=0x02"])
        args.extend(self.extra_args)
        if self.verbose:
            print(f"[harness] launching: {' '.join(args)}")
        self._lines = queue.Queue()
        self.proc = subprocess.Popen(
            args,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
            universal_newlines=True,
        )
        self._reader_thread = threading.Thread(target=self._reader_loop, daemon=True)
        self._reader_thread.start()
        self._ready = False
        self._exit_code = None
        self.wait_for_prompt(self.boot_timeout)

    def stop(self) -> None:
        if not self.proc:
            return
        if self.proc.poll() is None:
            try:
                self.proc.terminate()
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.proc = None
        self._ready = False
        self._lines = None

    def restart(self) -> None:
        self.stop()
        self.start()

    # ------------------------------------------------------------------
    # Serial handling
    # ------------------------------------------------------------------
    def _reader_loop(self) -> None:
        assert self.proc is not None
        stdout = self.proc.stdout
        assert stdout is not None
        assert self._lines is not None
        for raw_line in stdout:
            self._lines.put(raw_line.rstrip("\n"))
        if self.proc:
            self._exit_code = self.proc.poll()
        self._lines.put(None)

    def _next_line(self, deadline: Optional[float]) -> str:
        assert self._lines is not None
        while True:
            remaining: Optional[float]
            if deadline is None:
                remaining = None
            else:
                remaining = max(0.0, deadline - time.monotonic())
                if remaining <= 0.0:
                    raise HarnessTimeout("timed out waiting for serial output")
            try:
                item = self._lines.get(timeout=remaining)
            except queue.Empty as exc:
                raise HarnessTimeout("timed out waiting for serial output") from exc
            if item is None:
                raise HarnessProcessExit("kernel process exited unexpectedly", self._exit_code)
            return item.rstrip("\r")

    def _parse_line(self, line: str) -> Event:
        if not line:
            return Event(kind="log", payload=None, raw=line)
        if line.startswith("@@HARNESS "):
            payload = self._safe_json(line[len("@@HARNESS ") :], line)
            if payload is None:
                return Event(kind="log", payload=None, raw=line)
            return Event(kind="harness", payload=payload, raw=line)
        if line.startswith("@@TEST "):
            payload = self._safe_json(line[len("@@TEST ") :], line)
            if payload is None:
                return Event(kind="log", payload=None, raw=line)
            return Event(kind="test", payload=payload, raw=line)
        return Event(kind="log", payload=None, raw=line)

    @staticmethod
    def _safe_json(text: str, raw: str) -> Optional[Dict[str, Any]]:
        try:
            return json.loads(text)
        except json.JSONDecodeError:
            sys.stderr.write(f"[harness] failed to parse JSON from line: {raw}\n")
            return None

    def _next_event(self, deadline: Optional[float]) -> Event:
        line = self._next_line(deadline)
        event = self._parse_line(line)
        if self.verbose and event.kind != "log":
            sys.stdout.write(f"[harness] event: {event.raw}\n")
        return event if event.kind != "log" else Event(kind="log", payload=None, raw=line)

    # ------------------------------------------------------------------
    # Protocol helpers
    # ------------------------------------------------------------------
    def wait_for_prompt(self, timeout: float) -> None:
        if self._ready:
            return
        deadline = time.monotonic() + timeout
        while True:
            event = self._next_event(deadline)
            if event.kind == "harness" and event.payload.get("event") == "waiting":
                self._ready = True
                return

    def send_command(self, command: str) -> None:
        if not self.proc or self.proc.poll() is not None:
            raise HarnessProcessExit("kernel is not running")
        if not self._ready:
            raise HarnessError("attempted to send command while kernel busy")
        stdin = self.proc.stdin
        assert stdin is not None
        if self.verbose:
            print(f"[harness] -> {command}")
        stdin.write(command + "\r")
        stdin.flush()
        self._ready = False

    def gather_until_waiting(self, timeout: float) -> tuple[List[Event], List[str]]:
        deadline = time.monotonic() + timeout
        events: List[Event] = []
        raw_lines: List[str] = []
        while True:
            try:
                event = self._next_event(deadline)
            except HarnessProcessExit as exc:
                raise HarnessGuestExit(exc.exit_code if hasattr(exc, "exit_code") else None,
                                        events,
                                        raw_lines) from exc
            raw_lines.append(event.raw)
            if event.kind == "log":
                continue
            if event.kind == "harness" and event.payload.get("event") == "waiting":
                self._ready = True
                return events, raw_lines
            events.append(event)

    # ------------------------------------------------------------------
    # High-level operations
    # ------------------------------------------------------------------
    def list_tests(self, timeout: float) -> List[TestDescriptor]:
        self.wait_for_prompt(timeout)
        self.send_command("LIST")
        try:
            events, _ = self.gather_until_waiting(timeout)
        except HarnessGuestExit as exc:
            raise HarnessError(
                f"kernel exited while listing tests (status {exc.exit_code})"
            ) from exc
        descriptors: List[TestDescriptor] = []
        def _build_descriptor(payload: Dict[str, Any]) -> TestDescriptor:
            requires_clean = bool(payload.get("requires_clean_env"))
            return TestDescriptor(
                name=payload.get("name", ""),
                module=payload.get("module"),
                requires_clean_env=requires_clean,
                metadata=payload,
            )
        for event in events:
            if event.kind != "harness":
                continue
            payload = event.payload
            etype = payload.get("event")
            if etype == "test":
                descriptors.append(_build_descriptor(payload))
            elif etype == "list":
                tests = payload.get("tests", [])
                if isinstance(tests, list):
                    for item in tests:
                        if isinstance(item, dict):
                            descriptors.append(_build_descriptor(item))
                        else:
                            descriptors.append(TestDescriptor(name=str(item)))
            elif etype == "error":
                raise HarnessError(payload.get("message", "error during LIST"))
        return descriptors

    def run_test(
        self,
        test_name: str,
        timeout: float,
        retries: int,
        requires_clean_env: bool = False,
    ) -> TestResult:
        attempts: List[AttemptLog] = []
        max_attempts = 1 + max(0, retries)
        post_run_restart_done = False
        result: Optional[TestResult] = None
        if requires_clean_env:
            self.restart()
        for attempt_idx in range(1, max_attempts + 1):
            attempt_clean_env = requires_clean_env and attempt_idx == 1
            try:
                self.wait_for_prompt(timeout)
            except HarnessError as exc:
                attempts.append(
                    AttemptLog(
                        attempt=attempt_idx,
                        outcome="infra",
                        reason=str(exc),
                        requires_clean_env=attempt_clean_env,
                    )
                )
                self.restart()
                if requires_clean_env and attempt_idx == max_attempts:
                    post_run_restart_done = True
                continue

            self.send_command(f"RUN {test_name}")
            try:
                events, raw_lines = self.gather_until_waiting(timeout)
            except HarnessGuestExit as exc:
                exit_outcome, exit_reason = self._summarise_exit(exc.exit_code)
                event_outcome, event_reason, filtered_events = self._interpret_test_events(exc.events)

                def _priority(label: str) -> int:
                    return {
                        "error": 5,
                        "fail": 4,
                        "timeout": 3,
                        "infra": 2,
                        "skipped": 1,
                        "pass": 0,
                    }.get(label, 0)

                outcome = exit_outcome
                reason = exit_reason
                events_for_attempt = filtered_events or exc.events

                if event_outcome != "infra" and _priority(event_outcome) >= _priority(outcome):
                    outcome = event_outcome
                    reason = event_reason or reason

                error_messages: List[str] = []
                for event in exc.events:
                    payload = event.payload or {}
                    if payload.get("event") == "error":
                        message = payload.get("message") or payload.get("reason")
                        if message:
                            error_messages.append(message)

                if error_messages and outcome in ("pass", "infra"):
                    outcome = "fail"
                    reason = error_messages[0]
                elif not exc.events and outcome == "pass":
                    outcome = "infra"
                    reason = reason or "kernel exited without emitting events"

                attempts.append(
                    AttemptLog(
                        attempt=attempt_idx,
                        outcome=outcome,
                        reason=reason,
                        requires_clean_env=attempt_clean_env,
                        events=events_for_attempt,
                        lines=exc.lines,
                    )
                )
                # Restart the harness to be ready for subsequent tests.
                try:
                    self.restart()
                    if requires_clean_env:
                        post_run_restart_done = True
                except HarnessError:
                    pass
                result = TestResult(
                    name=test_name,
                    outcome=outcome,
                    requires_clean_env=requires_clean_env,
                    attempts=attempts,
                )
                break
            except HarnessTimeout as exc:
                attempts.append(
                    AttemptLog(
                        attempt=attempt_idx,
                        outcome="timeout",
                        reason=str(exc),
                        requires_clean_env=attempt_clean_env,
                    )
                )
                self.restart()
                if requires_clean_env and attempt_idx == max_attempts:
                    post_run_restart_done = True
                continue
            except HarnessProcessExit as exc:
                attempts.append(
                    AttemptLog(
                        attempt=attempt_idx,
                        outcome="infra",
                        reason=str(exc),
                        requires_clean_env=attempt_clean_env,
                    )
                )
                self.restart()
                if requires_clean_env and attempt_idx == max_attempts:
                    post_run_restart_done = True
                continue

            outcome, reason, filtered = self._interpret_test_events(events)
            attempts.append(
                AttemptLog(
                    attempt=attempt_idx,
                    outcome=outcome,
                    reason=reason,
                    requires_clean_env=attempt_clean_env,
                    events=filtered,
                    lines=raw_lines,
                )
            )
            if outcome == "pass" or outcome == "skipped" or outcome == "fail" or outcome == "error":
                result = TestResult(
                    name=test_name,
                    outcome=outcome,
                    requires_clean_env=requires_clean_env,
                    attempts=attempts,
                )
                break
            # Infrastructure issues fall through to retry.
            self.restart()
            if requires_clean_env and attempt_idx == max_attempts:
                post_run_restart_done = True

        if result is None:
            # All attempts exhausted without a definitive result.
            final_outcome = attempts[-1].outcome if attempts else "infra"
            result = TestResult(
                name=test_name,
                outcome=final_outcome,
                requires_clean_env=requires_clean_env,
                attempts=attempts,
            )

        if requires_clean_env and not post_run_restart_done:
            try:
                self.restart()
            except HarnessError:
                pass

        return result

    @staticmethod
    def _summarise_exit(exit_code: Optional[int]) -> tuple[str, str]:
        if exit_code is None:
            return "error", "kernel exited without exit code"
        if exit_code < 0:
            return "error", f"kernel terminated by signal {-exit_code}"
        if exit_code & 1:
            guest_code = exit_code >> 1
            return "error", f"guest requested abort via isa-debug-exit (code {guest_code})"
        if exit_code == 0:
            return "pass", "guest exited cleanly"
        return "infra", f"unexpected kernel exit status {exit_code}"

    def _interpret_test_events(self, events: Iterable[Event]) -> tuple[str, Optional[str], List[Event]]:
        outcome = "infra"
        reason: Optional[str] = None
        filtered: List[Event] = []
        started = False
        ended = False
        error_seen = False
        error_reason: Optional[str] = None
        for event in events:
            filtered.append(event)
            payload = event.payload or {}
            if event.kind == "harness":
                etype = payload.get("event")
                if etype == "test_start":
                    started = True
                elif etype == "test_end":
                    ended = True
                    outcome = payload.get("status", "pass")
                    reason = payload.get("reason")
                elif etype == "error":
                    error_seen = True
                    error_reason = payload.get("message")
                    status = payload.get("status")
                    if status:
                        outcome = status
                    reason = error_reason
                    # an error does not necessarily end the stream; keep reading
            elif event.kind == "test":
                etype = payload.get("event")
                if etype == "test_start":
                    started = True
                elif etype == "test_end":
                    ended = True
                    outcome = payload.get("status", "pass")
                    reason = payload.get("reason")
                elif etype == "error":
                    error_seen = True
                    error_reason = payload.get("reason") or payload.get("message")
                    status = payload.get("status")
                    if status:
                        outcome = status
                    reason = error_reason or reason
        if not started and outcome == "infra":
            reason = "test did not start"
        if not ended and outcome in ("pass", "infra"):
            outcome = "timeout"
            if reason is None:
                reason = "did not observe test_end"
        if error_seen and outcome in ("pass", "skipped"):
            outcome = "fail"
            if reason is None:
                reason = error_reason or "error event emitted"
        elif error_seen and outcome == "infra":
            outcome = "error"
            if reason is None:
                reason = error_reason or "error event emitted"
        if outcome not in ("pass", "fail", "error", "skipped", "timeout"):
            outcome = "infra"
        return outcome, reason, filtered


# ----------------------------------------------------------------------
# Artifact helpers
# ----------------------------------------------------------------------
def write_artifacts(base_dir: Optional[Path], result: TestResult) -> None:
    if base_dir is None:
        return
    case_dir = base_dir / result.name
    case_dir.mkdir(parents=True, exist_ok=True)

    final_attempt = result.final_attempt
    console_path = case_dir / "console.log"
    with console_path.open("w", encoding="utf-8") as handle:
        for line in final_attempt.lines:
            handle.write(line)
            handle.write("\n")

    events_path = case_dir / "events.jsonl"
    with events_path.open("w", encoding="utf-8") as handle:
        for event in final_attempt.events:
            if event.payload is not None:
                json.dump(event.payload, handle)
                handle.write("\n")

    meta = {
        "test": result.name,
        "outcome": result.outcome,
        "requires_clean_env": result.requires_clean_env,
        "attempts": [
            {
                "attempt": attempt.attempt,
                "outcome": attempt.outcome,
                "reason": attempt.reason,
                "requires_clean_env": attempt.requires_clean_env,
            }
            for attempt in result.attempts
        ],
    }
    meta_path = case_dir / "harness.json"
    with meta_path.open("w", encoding="utf-8") as handle:
        json.dump(meta, handle, indent=2)
        handle.write("\n")


# ----------------------------------------------------------------------
# Scheduling helpers
# ----------------------------------------------------------------------
def reorder_tests_for_execution(tests: List[str], descriptor_map: Dict[str, TestDescriptor]) -> List[str]:
    module_order: List[str] = []
    module_buckets: Dict[str, Dict[str, deque[str]]] = {}
    known_count = 0

    for test in tests:
        descriptor = descriptor_map.get(test)
        if descriptor is None:
            continue
        known_count += 1
        module_key = descriptor.module or ""
        if module_key not in module_buckets:
            module_buckets[module_key] = {"normal": deque(), "clean": deque()}
            module_order.append(module_key)
        bucket = module_buckets[module_key]
        target = "clean" if descriptor.requires_clean_env else "normal"
        bucket[target].append(test)

    scheduled_known: List[str] = []

    while True:
        progress = False
        for module in module_order:
            normal_bucket = module_buckets[module]["normal"]
            if normal_bucket:
                scheduled_known.append(normal_bucket.popleft())
                progress = True
        if not progress:
            break

    clean_modules = [module for module in module_order if module_buckets[module]["clean"]]
    while True:
        progress = False
        for module in clean_modules:
            clean_bucket = module_buckets[module]["clean"]
            if clean_bucket:
                scheduled_known.append(clean_bucket.popleft())
                progress = True
        if not progress:
            break

    if len(scheduled_known) != known_count:
        for module in module_order:
            for bucket_name in ("normal", "clean"):
                bucket = module_buckets[module][bucket_name]
                while bucket:
                    scheduled_known.append(bucket.popleft())

    scheduled_iter = iter(scheduled_known)
    reordered: List[str] = []
    for test in tests:
        if descriptor_map.get(test) is None:
            reordered.append(test)
        else:
            reordered.append(next(scheduled_iter))
    return reordered


# ----------------------------------------------------------------------
# CLI glue
# ----------------------------------------------------------------------
def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run Archipelago kernel tests under QEMU")
    parser.add_argument("tests", nargs="*", help="Specific tests to run (defaults to all)")
    parser.add_argument("--iso", default="build/image.iso", type=Path, help="Path to the Archipelago ISO")
    parser.add_argument("--qemu", default="qemu-system-x86_64", help="QEMU binary to use")
    parser.add_argument("--memory", type=int, default=64, help="Guest memory size in MiB")
    parser.add_argument("--boot-timeout", type=float, default=30.0, help="Timeout for kernel to enter waiting state")
    parser.add_argument("--command-timeout", type=float, default=5.0, help="Timeout for list and run commands")
    parser.add_argument("--test-timeout", type=float, default=5.0, help="Timeout waiting for a test to finish")
    parser.add_argument("--retries", type=int, default=3, help="Retries for infrastructure failures")
    parser.add_argument("--list", action="store_true", help="List tests and exit")
    parser.add_argument("--artifacts", type=Path, default=Path("build/test-artifacts"), help="Directory for test artifacts")
    parser.add_argument("--qemu-arg", action="append", default=[], help="Additional argument to pass to QEMU (repeatable)")
    parser.add_argument("--no-artifacts", action="store_true", help="Disable artifact generation")
    parser.add_argument("--no-exit-device", action="store_true", help="Do not attach isa-debug-exit to QEMU")
    parser.add_argument("--verbose", action="store_true", help="Verbose harness logging")
    return parser.parse_args(argv)


def ensure_iso(path: Path) -> None:
    if not path.exists():
        raise HarnessError(f"ISO image not found: {path}")


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)

    ensure_iso(args.iso)
    artifact_dir: Optional[Path]
    if args.no_artifacts:
        artifact_dir = None
    else:
        artifact_dir = args.artifacts
        artifact_dir.mkdir(parents=True, exist_ok=True)

    harness = KernelHarness(
        qemu=args.qemu,
        iso=args.iso,
        memory=args.memory,
        boot_timeout=args.boot_timeout,
        extra_args=args.qemu_arg,
        verbose=args.verbose,
        add_exit_device=not args.no_exit_device,
    )

    exit_code = 0
    try:
        harness.start()

        descriptors = harness.list_tests(args.command_timeout)
        descriptor_map = {descriptor.name: descriptor for descriptor in descriptors}

        if args.list and not args.tests:
            for descriptor in descriptors:
                module = f" [{descriptor.module}]" if descriptor.module else ""
                print(f"{descriptor.name}{module}")
            return 0

        tests_to_run: List[str]
        if args.tests:
            tests_to_run = list(dict.fromkeys(args.tests))
            for test_name in tests_to_run:
                if test_name not in descriptor_map:
                    print(
                        f"[harness] warning: test '{test_name}' not discovered via LIST",
                        file=sys.stderr,
                    )
        else:
            tests_to_run = [descriptor.name for descriptor in descriptors]
            if not tests_to_run:
                print("[harness] no tests discovered", file=sys.stderr)
                return 1

        tests_to_run = reorder_tests_for_execution(tests_to_run, descriptor_map)

        results: List[TestResult] = []
        failure_summaries: List[str] = []
        passed_count = 0
        failed_count = 0

        for test_name in tests_to_run:
            descriptor = descriptor_map.get(test_name)
            requires_clean_env = descriptor.requires_clean_env if descriptor else False
            result = harness.run_test(
                test_name,
                timeout=args.test_timeout,
                retries=args.retries,
                requires_clean_env=requires_clean_env,
            )
            results.append(result)
            write_artifacts(artifact_dir, result)

            if result.outcome in ("pass", "skipped"):
                passed_count += 1
            else:
                failed_count += 1
                attempt = result.final_attempt
                prefix = {
                    "fail": "FAIL",
                    "error": "ERROR",
                    "timeout": "TIMEOUT",
                    "infra": "INFRA",
                }.get(result.outcome, result.outcome.upper())
                summary = f"{prefix}: {test_name} (attempts: {result.attempts_count})"
                error_messages: List[str] = []
                for event in attempt.events:
                    payload = event.payload or {}
                    if payload.get("event") == "error":
                        message = payload.get("message") or payload.get("reason")
                        if message:
                            error_messages.append(message)
                if attempt.reason and not error_messages:
                    error_messages.append(attempt.reason)
                if error_messages:
                    summary += "\n" + "\n".join(f"    - {msg}" for msg in error_messages)
                failure_summaries.append(summary)

        if failure_summaries:
            for summary in failure_summaries:
                print(summary)
        else:
            print("No test failures.")

        print(f"Summary: {passed_count} passed, {failed_count} failed")

        if failed_count > 0:
            exit_code = 1

    except (HarnessError, HarnessProcessExit) as exc:
        print(f"[harness] error: {exc}", file=sys.stderr)
        exit_code = 1
    except KeyboardInterrupt:
        print("[harness] interrupted", file=sys.stderr)
        exit_code = 130
    finally:
        harness.stop()
    return exit_code


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
