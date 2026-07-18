#!/usr/bin/env python3
"""Kernel testing harness that drives the Archipelago test mode over QEMU serial."""

from __future__ import annotations

import argparse
import json
import os
import queue
import shutil
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness_protocol  # noqa: E402 -- shared @@HARNESS result schema (one schema across both tiers)


# Outcome priority for resolving conflicts between exit-code and event outcomes.
_OUTCOME_PRIORITY = {"error": 5, "fail": 4, "timeout": 3, "infra": 2, "skipped": 1, "pass": 0}

# Prefixes recognised on the serial line, mapped to event kind.
_EVENT_PREFIXES = (("@@HARNESS ", "harness"), ("@@TEST ", "test"))

# @@CRASH_<SECTION> {json} -- multi-line crash dump. Sections: BEGIN, REG, FRAME, STACK, LOG, END.
_CRASH_PREFIX = "@@CRASH_"


@dataclass
class Event:
    kind: str  # 'harness', 'test', or 'log'
    payload: Optional[Dict[str, Any]]
    raw: str


@dataclass
class Crash:
    trigger: str = ""
    vec: Optional[int] = None
    err: Optional[int] = None
    test: Optional[str] = None
    message: Optional[str] = None
    file: Optional[str] = None
    line: Optional[int] = None
    ts: Optional[int] = None
    cpu: Optional[int] = None
    registers: Dict[str, str] = field(default_factory=dict)
    frames: List[Dict[str, Any]] = field(default_factory=list)
    stack: Optional[Dict[str, Any]] = None
    log: List[Dict[str, Any]] = field(default_factory=list)
    raw_lines: List[str] = field(default_factory=list)


@dataclass
class AttemptLog:
    attempt: int
    outcome: str  # pass, fail, error, timeout, infra
    reason: Optional[str]
    requires_clean_env: bool = False
    events: List[Event] = field(default_factory=list)
    lines: List[str] = field(default_factory=list)
    crash: Optional[Crash] = None
    duration_ns: Optional[int] = None


@dataclass
class TestResult:
    name: str
    outcome: str
    module: Optional[str] = None
    requires_clean_env: bool = False
    expects_crash: bool = False
    attempts: List[AttemptLog] = field(default_factory=list)
    duration_ns: Optional[int] = None


@dataclass
class TestDescriptor:
    name: str
    module: Optional[str] = None
    requires_clean_env: bool = False
    expects_crash: bool = False
    metadata: Dict[str, Any] = field(default_factory=dict)


class HarnessError(Exception):
    pass


class HarnessProcessExit(HarnessError):
    def __init__(self, message: str, exit_code: Optional[int] = None) -> None:
        super().__init__(message)
        self.exit_code = exit_code


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
        arch: str = "x86_64",
        firmware: Optional[Path] = None,
    ) -> None:
        self.qemu = qemu
        self.iso = iso
        self.memory = memory
        self.boot_timeout = boot_timeout
        self.extra_args = list(extra_args or [])
        self.verbose = verbose
        self.add_exit_device = add_exit_device
        self.arch = arch
        self.firmware = firmware

        self.proc: Optional[subprocess.Popen[str]] = None
        self._reader_thread: Optional[threading.Thread] = None
        self._lines: Optional[queue.Queue[Optional[str]]] = None
        self._ready = False
        self._protocol_enabled = False
        self._exit_code: Optional[int] = None

    # ------------------------------------------------------------------
    # Process lifecycle
    # ------------------------------------------------------------------
    def start(self) -> None:
        if self.proc and self.proc.poll() is None:
            return
        args = [
            self.qemu,
            "-serial", "stdio", "-display", "none", "-no-reboot",
            "-m", str(self.memory),
        ]
        args.extend(machine_args(self.arch, self.iso, self.firmware, exit_device=self.add_exit_device))
        args.extend(self.extra_args)
        if self.verbose:
            print(f"[harness] launching: {' '.join(args)}")
        self._lines = queue.Queue()
        self.proc = subprocess.Popen(
            args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, bufsize=1, universal_newlines=True,
        )
        self._reader_thread = threading.Thread(target=self._reader_loop, daemon=True)
        self._reader_thread.start()
        self._ready = False
        self._protocol_enabled = False
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
        assert self.proc is not None and self.proc.stdout is not None and self._lines is not None
        lines = self._lines  # capture: stop() may null self._lines during teardown
        for raw_line in self.proc.stdout:
            lines.put(raw_line.rstrip("\n"))
        if self.proc:
            self._exit_code = self.proc.poll()
        lines.put(None)

    def _next_line(self, deadline: Optional[float]) -> str:
        assert self._lines is not None
        remaining = None if deadline is None else max(0.0, deadline - time.monotonic())
        if remaining is not None and remaining <= 0.0:
            raise HarnessError("timed out waiting for serial output")
        try:
            item = self._lines.get(timeout=remaining)
        except queue.Empty:
            raise HarnessError("timed out waiting for serial output")
        if item is None:
            raise HarnessProcessExit("kernel process exited unexpectedly", self._exit_code)
        return item.rstrip("\r")

    def _parse_line(self, line: str) -> Event:
        for prefix, kind in _EVENT_PREFIXES:
            if line.startswith(prefix):
                try:
                    return Event(kind=kind, payload=json.loads(line[len(prefix):]), raw=line)
                except json.JSONDecodeError:
                    sys.stderr.write(f"[harness] failed to parse JSON from line: {line}\n")
                    break
        if line.startswith(_CRASH_PREFIX):
            space = line.find(" ", len(_CRASH_PREFIX))
            section = line[len(_CRASH_PREFIX):space if space > 0 else len(line)].lower()
            payload: Dict[str, Any] = {"section": section}
            if space > 0:
                try:
                    payload.update(json.loads(line[space + 1:]))
                except json.JSONDecodeError:
                    sys.stderr.write(f"[harness] failed to parse crash JSON: {line}\n")
            return Event(kind="crash", payload=payload, raw=line)
        return Event(kind="log", payload=None, raw=line)

    def _next_event(self, deadline: Optional[float]) -> Event:
        event = self._parse_line(self._next_line(deadline))
        if self.verbose and event.kind != "log":
            sys.stdout.write(f"[harness] event: {event.raw}\n")
        return event

    # ------------------------------------------------------------------
    # Protocol helpers
    # ------------------------------------------------------------------
    def wait_for_prompt(self, timeout: float) -> None:
        if self._ready:
            return
        deadline = time.monotonic() + timeout
        if not self._protocol_enabled:
            self._enable_protocol(deadline)
        else:
            self._wait_for_ready(deadline)

    def _enable_protocol(self, deadline: float) -> None:
        settle_timeout = 0.5
        seen_output = False
        while True:
            inner_deadline = min(deadline, time.monotonic() + settle_timeout) if seen_output else deadline
            try:
                line = self._next_line(inner_deadline)
                seen_output = True
            except HarnessError:
                if seen_output:
                    break
                raise HarnessError("timed out waiting for shell prompt")
            except HarnessProcessExit:
                raise
            event = self._parse_line(line)
            if event.kind == "harness" and event.payload and event.payload.get("event") in ("waiting", "ready"):
                self._ready = True
                self._protocol_enabled = True
                return

        assert self.proc is not None and self.proc.stdin is not None
        # The settle heuristic can fire during a firmware pause (EDK2's 5-second
        # ESC window on riscv64), in which case the firmware eats the command
        # before the kernel shell exists. Resend on a generous interval until
        # the kernel acknowledges or the boot deadline passes; QEMU applies
        # serial backpressure, so an early command queues rather than drops.
        self._protocol_enabled = True
        while True:
            if self.verbose:
                print("[harness] -> harness enable")
            self.proc.stdin.write("harness enable\r")
            self.proc.stdin.flush()
            inner_deadline = min(deadline, time.monotonic() + 5.0)
            try:
                self._wait_for_ready(inner_deadline)
                self._drain_stale_ready()
                return
            except HarnessProcessExit:
                raise
            except HarnessError:
                if time.monotonic() >= deadline:
                    raise HarnessError("timed out waiting for harness ready")

    def _drain_stale_ready(self) -> None:
        # Every resent enable that reached the shell produces its own ready;
        # consume the stragglers until the line goes quiet so a later
        # gather_until_waiting doesn't mistake one for command completion.
        while True:
            try:
                self._next_event(time.monotonic() + 1.0)
            except HarnessError:
                return

    def _wait_for_ready(self, deadline: float) -> None:
        while True:
            event = self._next_event(deadline)
            if event.kind == "harness" and event.payload and event.payload.get("event") in ("waiting", "ready"):
                self._ready = True
                return

    def send_command(self, command: str) -> None:
        if not self.proc or self.proc.poll() is not None:
            raise HarnessProcessExit("kernel is not running")
        if not self._ready:
            raise HarnessError("attempted to send command while kernel busy")
        assert self.proc.stdin is not None
        if self.verbose:
            print(f"[harness] -> {command}")
        self.proc.stdin.write(command + "\r")
        self.proc.stdin.flush()
        self._ready = False

    def gather_until_waiting(self, timeout: float) -> tuple[List[Event], List[str]]:
        deadline = time.monotonic() + timeout
        events: List[Event] = []
        raw_lines: List[str] = []
        while True:
            try:
                event = self._next_event(deadline)
            except HarnessProcessExit as exc:
                # Return what we have; caller interprets the exit.
                return events, raw_lines
            raw_lines.append(event.raw)
            if event.kind == "log":
                continue
            if event.kind == "harness" and event.payload and event.payload.get("event") in ("waiting", "ready"):
                self._ready = True
                return events, raw_lines
            events.append(event)

    # ------------------------------------------------------------------
    # High-level operations
    # ------------------------------------------------------------------
    def list_tests(self, timeout: float) -> List[TestDescriptor]:
        self.wait_for_prompt(timeout)
        self.send_command("test list")
        events, _ = self.gather_until_waiting(timeout)
        descriptors: List[TestDescriptor] = []

        def _build(payload: Dict[str, Any]) -> TestDescriptor:
            return TestDescriptor(
                name=payload.get("name", ""),
                module=payload.get("module"),
                requires_clean_env=bool(payload.get("requires_clean_env")),
                expects_crash=bool(payload.get("expects_crash")),
                metadata=payload,
            )

        for event in events:
            if event.kind != "harness" or not event.payload:
                continue
            etype = event.payload.get("event")
            if etype == "test":
                descriptors.append(_build(event.payload))
            elif etype == "list":
                tests = event.payload.get("tests", [])
                for item in (tests if isinstance(tests, list) else []):
                    descriptors.append(_build(item) if isinstance(item, dict) else TestDescriptor(name=str(item)))
            elif etype == "error":
                raise HarnessError(event.payload.get("message", "error during LIST"))
        return descriptors

    def run_test(
        self,
        test_name: str,
        timeout: float,
        retries: int,
        requires_clean_env: bool = False,
        expects_crash: bool = False,
        module: Optional[str] = None,
    ) -> TestResult:
        attempts: List[AttemptLog] = []
        max_attempts = 1 + max(0, retries)

        if requires_clean_env:
            self.restart()

        for attempt_idx in range(1, max_attempts + 1):
            attempt = self._run_single_attempt(test_name, timeout, attempt_idx, requires_clean_env)
            if expects_crash:
                attempt = _invert_for_expected_crash(attempt)
            attempts.append(attempt)

            if attempt.outcome in ("pass", "skipped", "fail", "error"):
                break
            # Infrastructure/timeout: restart and retry
            self.restart()
        else:
            attempt = None  # all attempts exhausted via loop

        if requires_clean_env:
            try:
                self.restart()
            except HarnessError:
                pass

        final_outcome = attempts[-1].outcome if attempts else "infra"
        return TestResult(
            name=test_name,
            outcome=final_outcome,
            module=module,
            requires_clean_env=requires_clean_env,
            expects_crash=expects_crash,
            attempts=attempts,
            duration_ns=attempts[-1].duration_ns if attempts else None,
        )

    def _run_single_attempt(
        self, test_name: str, timeout: float, attempt_idx: int, requires_clean_env: bool
    ) -> AttemptLog:
        clean_flag = requires_clean_env and attempt_idx == 1
        try:
            self.wait_for_prompt(timeout)
        except HarnessError as exc:
            return AttemptLog(attempt=attempt_idx, outcome="infra", reason=str(exc), requires_clean_env=clean_flag)

        self.send_command(f"test run {test_name}")
        try:
            events, raw_lines = self.gather_until_waiting(timeout)
        except HarnessError as exc:
            return AttemptLog(attempt=attempt_idx, outcome="infra", reason=str(exc), requires_clean_env=clean_flag)

        outcome, reason, filtered = self._interpret_test_events(events)
        crash = self._aggregate_crash(events)

        # If kernel exited (gather returned early without "waiting"), check exit code
        if not self._ready:
            exit_outcome, exit_reason = self._summarise_exit(self._exit_code)
            # Use whichever outcome is more severe
            if _OUTCOME_PRIORITY.get(exit_outcome, 0) > _OUTCOME_PRIORITY.get(outcome, 0):
                outcome, reason = exit_outcome, exit_reason
            # Error messages from events override generic exit reasons
            error_msgs = [
                (e.payload.get("message") or e.payload.get("reason"))
                for e in events if e.payload and e.payload.get("event") == "error"
            ]
            error_msgs = [m for m in error_msgs if m]
            if error_msgs and outcome in ("pass", "infra"):
                outcome, reason = "fail", error_msgs[0]
            elif not events and outcome == "pass":
                outcome = "infra"
                reason = reason or "kernel exited without emitting events"

        return AttemptLog(
            attempt=attempt_idx, outcome=outcome, reason=reason,
            requires_clean_env=clean_flag, events=filtered, lines=raw_lines,
            crash=crash, duration_ns=self._extract_duration(events),
        )

    def _summarise_exit(self, exit_code: Optional[int]) -> tuple[str, str]:
        if exit_code is None:
            return "error", "kernel exited without exit code"
        if exit_code < 0:
            return "error", f"kernel terminated by signal {-exit_code}"
        if self.arch == "riscv64":
            # sifive_test finisher: PASS exits 0, FAIL exits with the guest's
            # code verbatim. A QEMU-internal failure also exits 1, which is
            # indistinguishable from guest code 1 -- acceptable, both are fatal.
            if exit_code == 0:
                return "pass", "guest exited cleanly"
            return "error", f"guest requested abort via sifive_test finisher (code {exit_code})"
        if exit_code & 1:
            return "error", f"guest requested abort via isa-debug-exit (code {exit_code >> 1})"
        if exit_code == 0:
            return "pass", "guest exited cleanly"
        return "infra", f"unexpected kernel exit status {exit_code}"

    @staticmethod
    def _extract_duration(events: list[Event]) -> Optional[int]:
        """Per-test duration in ns from the kernel's test_start/test_end timestamps."""
        start_ts = end_ts = None
        for event in events:
            payload = event.payload or {}
            etype = payload.get("event")
            if etype == "test_start" and isinstance(payload.get("timestamp"), int):
                start_ts = payload["timestamp"]
            elif etype in ("test_end", "test_crash") and isinstance(payload.get("timestamp"), int):
                end_ts = payload["timestamp"]
        if start_ts is not None and end_ts is not None and end_ts >= start_ts:
            return end_ts - start_ts
        return None

    @staticmethod
    def _interpret_test_events(events: list[Event]) -> tuple[str, Optional[str], List[Event]]:
        outcome, reason = "infra", None
        filtered: List[Event] = []
        started = ended = error_seen = crash_seen = False
        error_reason: Optional[str] = None

        for event in events:
            filtered.append(event)
            payload = event.payload or {}
            if event.kind not in ("harness", "test"):
                continue
            etype = payload.get("event")
            if etype == "test_start":
                started = True
            elif etype == "test_end":
                ended = True
                outcome = payload.get("status", "pass")
                reason = payload.get("reason")
            elif etype == "test_crash":
                crash_seen = True
                ended = True
                outcome = "fail"
                trig = payload.get("trigger", "crash")
                reason = f"crash: {trig}"
            elif etype == "error":
                error_seen = True
                error_reason = payload.get("message") or payload.get("reason")
                if status := payload.get("status"):
                    outcome = status
                reason = error_reason or reason

        if not started and outcome == "infra":
            reason = "test did not start"
        if not ended and outcome in ("pass", "infra"):
            outcome = "timeout"
            reason = reason or "did not observe test_end"
        if error_seen and not crash_seen:
            if outcome in ("pass", "skipped"):
                outcome = "fail"
                reason = reason or error_reason or "error event emitted"
            elif outcome == "infra":
                outcome = "error"
                reason = reason or error_reason or "error event emitted"
        if outcome not in ("pass", "fail", "error", "skipped", "timeout"):
            outcome = "infra"
        return outcome, reason, filtered

    @staticmethod
    def _aggregate_crash(events: list[Event]) -> Optional[Crash]:
        crash: Optional[Crash] = None
        in_crash = False
        for event in events:
            if event.kind != "crash":
                continue
            payload = event.payload or {}
            section = payload.get("section", "")
            if section == "begin":
                crash = Crash(
                    trigger=payload.get("trigger", ""),
                    vec=payload.get("vec"),
                    err=payload.get("err"),
                    test=payload.get("test"),
                    message=payload.get("message"),
                    file=payload.get("file"),
                    line=payload.get("line"),
                    ts=payload.get("ts"),
                    cpu=payload.get("cpu"),
                )
                in_crash = True
            elif not in_crash or crash is None:
                continue
            elif section == "reg":
                crash.registers = {k: v for k, v in payload.items() if k != "section"}
            elif section == "frame":
                crash.frames.append({"i": payload.get("i"), "addr": payload.get("addr")})
            elif section == "stack":
                crash.stack = {k: v for k, v in payload.items() if k != "section"}
            elif section == "log":
                crash.log.append({k: v for k, v in payload.items() if k != "section"})
            elif section == "end":
                in_crash = False
            if crash is not None:
                crash.raw_lines.append(event.raw)
        return crash


# ----------------------------------------------------------------------
# Outcome inversion for expects_crash tests
# ----------------------------------------------------------------------
def _invert_for_expected_crash(attempt: AttemptLog) -> AttemptLog:
    """For tests marked KTEST_FLAG_EXPECTS_CRASH, a crash is the desired outcome.

    Inverts pass/fail accordingly: crash -> pass, clean exit -> fail.
    Infra/timeout still surface as their original outcome (the harness itself
    misbehaved; that's not the test's fault).
    """
    if attempt.crash is not None:
        # Crash captured -- this is what the test wanted.
        attempt.outcome = "pass"
        attempt.reason = f"expected crash captured ({attempt.crash.trigger})"
    elif attempt.outcome == "pass":
        # Test ran to completion without crashing -- the test failed to crash.
        attempt.outcome = "fail"
        attempt.reason = "expected crash, but test completed normally"
    return attempt


# ----------------------------------------------------------------------
# Symbolication
# ----------------------------------------------------------------------
def _addr2line(kernel_elf: Path, addrs: List[str]) -> Dict[str, str]:
    """Resolve hex addresses to 'function at file:line' via llvm-addr2line.

    Returns a mapping addr -> resolved string. Missing/failed addresses map to "??".
    """
    result: Dict[str, str] = {a: "??" for a in addrs if a}
    addrs = [a for a in addrs if a]
    if not addrs or not kernel_elf.exists():
        return result
    binary = shutil.which("llvm-addr2line") or shutil.which("addr2line")
    if not binary:
        return result
    try:
        proc = subprocess.run(
            [binary, "-e", str(kernel_elf), "-f", "-p", "-C", *addrs],
            capture_output=True, text=True, check=False, timeout=30,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        sys.stderr.write(f"[harness] addr2line failed: {exc}\n")
        return result
    lines = [ln for ln in proc.stdout.splitlines() if ln]
    for addr, line in zip(addrs, lines):
        result[addr] = line.strip()
    return result


def _pc_key(crash: Crash) -> str:
    # x86_64 emits rip, riscv64 emits pc.
    return "rip" if "rip" in crash.registers else "pc"


def symbolicate_crash(crash: Crash, kernel_elf: Path) -> None:
    addrs: List[str] = []
    pc_key = _pc_key(crash)
    pc = crash.registers.get(pc_key)
    if pc:
        addrs.append(pc)
    for frame in crash.frames:
        addr = frame.get("addr")
        if addr:
            addrs.append(addr)
    resolved = _addr2line(kernel_elf, addrs)
    if pc and pc in resolved:
        crash.registers[pc_key + "_symbol"] = resolved[pc]
    for frame in crash.frames:
        addr = frame.get("addr")
        if addr and addr in resolved:
            frame["symbol"] = resolved[addr]


# ----------------------------------------------------------------------
# Artifact helpers
# ----------------------------------------------------------------------
def write_artifacts(base_dir: Optional[Path], result: TestResult) -> None:
    if base_dir is None:
        return
    artifact_root = base_dir
    if result.module:
        for part in result.module.split("/"):
            if part not in ("", ".", ".."):
                artifact_root = artifact_root / part
    case_dir = artifact_root / result.name
    case_dir.mkdir(parents=True, exist_ok=True)

    final = result.attempts[-1]
    (case_dir / "console.log").write_text("\n".join(final.lines) + "\n" if final.lines else "", encoding="utf-8")
    (case_dir / "events.jsonl").write_text(
        "".join(json.dumps(e.payload) + "\n" for e in final.events if e.payload is not None),
        encoding="utf-8",
    )
    meta = {
        "test": result.name, "outcome": result.outcome, "module": result.module,
        "requires_clean_env": result.requires_clean_env,
        "attempts": [
            {"attempt": a.attempt, "outcome": a.outcome, "reason": a.reason, "requires_clean_env": a.requires_clean_env}
            for a in result.attempts
        ],
        "crash": _crash_summary(final.crash) if final.crash else None,
    }
    (case_dir / "harness.json").write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8")
    if final.crash is not None:
        _write_crash_artifacts(case_dir, final.crash)


def _crash_summary(crash: Crash) -> Dict[str, Any]:
    return {
        "trigger": crash.trigger, "vec": crash.vec, "err": crash.err, "test": crash.test,
        "message": crash.message, "file": crash.file, "line": crash.line,
        "pc": crash.registers.get(_pc_key(crash)), "pc_symbol": crash.registers.get(_pc_key(crash) + "_symbol"),
        "frame_count": len(crash.frames), "log_count": len(crash.log),
    }


def _write_crash_artifacts(case_dir: Path, crash: Crash) -> None:
    payload = {
        "trigger": crash.trigger, "vec": crash.vec, "err": crash.err,
        "test": crash.test, "message": crash.message, "file": crash.file, "line": crash.line,
        "ts": crash.ts, "cpu": crash.cpu,
        "registers": crash.registers, "frames": crash.frames,
        "stack": crash.stack, "log": crash.log,
    }
    (case_dir / "crash.json").write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")

    text_lines = ["*** KERNEL CRASH ***"]
    text_lines.append(f"Trigger: {crash.trigger}" + (f" (vec={crash.vec}, err={crash.err})" if crash.vec is not None else ""))
    if crash.message:
        text_lines.append(f"Message: {crash.message}")
    if crash.file:
        text_lines.append(f"At:      {crash.file}:{crash.line}")
    if crash.test:
        text_lines.append(f"Test:    {crash.test}")
    text_lines.append("")
    if crash.registers:
        text_lines.append("Registers:")
        pc_key = _pc_key(crash)
        if pc_sym := crash.registers.get(pc_key + "_symbol"):
            text_lines.append(f"  {pc_key} = {crash.registers.get(pc_key)}  ({pc_sym})")
        else:
            text_lines.append(f"  {pc_key} = {crash.registers.get(pc_key)}")
        # Whatever the kernel emitted, in its own order -- no per-arch list here.
        rest = [n for n in crash.registers if n not in (pc_key, pc_key + "_symbol")]
        for i in range(0, len(rest), 4):
            text_lines.append("  " + "  ".join(f"{n}={crash.registers[n]}" for n in rest[i:i + 4]))
        text_lines.append("")
    if crash.frames:
        text_lines.append(f"Backtrace ({len(crash.frames)} frames):")
        for frame in crash.frames:
            sym = frame.get("symbol", "??")
            text_lines.append(f"  [{frame.get('i')}] {frame.get('addr')}  {sym}")
        text_lines.append("")
    if crash.stack:
        text_lines.append(f"Stack ({crash.stack.get('len')} bytes from {crash.stack.get('rsp')}):")
        text_lines.append(f"  {crash.stack.get('hex', '')}")
        text_lines.append("")
    if crash.log:
        text_lines.append(f"Recent log ({len(crash.log)} entries):")
        for entry in crash.log:
            ns = entry.get("ns", 0)
            sec = ns // 1_000_000_000
            ms  = (ns // 1_000_000) % 1000
            text_lines.append(f"  [{sec:03d}.{ms:03d} {entry.get('lvl', '?')}] {entry.get('text', '')}")
    (case_dir / "crash.txt").write_text("\n".join(text_lines) + "\n", encoding="utf-8")


# ----------------------------------------------------------------------
# Shared-schema bridge -- one result schema across the host and QEMU tiers
# ----------------------------------------------------------------------
def _to_protocol_result(result: TestResult) -> harness_protocol.TestResult:
    """Project the rich QEMU TestResult onto the shared {id, tier, outcome, duration_ns, failures,
    diagnostics} schema, so both tiers report through the same structure."""
    pr = harness_protocol.TestResult(result.name, "qemu")
    pr.outcome = "pass" if result.outcome in ("pass", "skipped") else "fail"
    pr.duration_ns = result.duration_ns
    final = result.attempts[-1] if result.attempts else None
    failures: List[str] = []
    if final:
        for event in final.events:
            payload = event.payload or {}
            if payload.get("event") == "error":
                msg = payload.get("message") or payload.get("reason")
                if msg:
                    failures.append(msg)
        if not failures and pr.outcome == "fail" and final.reason:
            failures.append(final.reason)
    pr.failures = failures
    # The QEMU tier carries a richer outcome and crash data than the common schema; keep them as diagnostics.
    pr.diagnostics = {
        "detailed_outcome": result.outcome,
        "module": result.module,
        "attempts": len(result.attempts),
        "expects_crash": result.expects_crash,
    }
    if final and final.crash is not None:
        pr.diagnostics["crash"] = _crash_summary(final.crash)
    return pr


def write_summary(base_dir: Optional[Path], proto_results: List[harness_protocol.TestResult]) -> None:
    """Top-level summary.json in the shared schema -- mirrors the host tier's harness.json."""
    if base_dir is None:
        return
    passed = sum(1 for r in proto_results if r.outcome == "pass")
    summary = {
        "tier": "qemu", "total": len(proto_results), "passed": passed,
        "failed": len(proto_results) - passed,
        "results": [r.to_dict() for r in proto_results],
    }
    (base_dir / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    harness_protocol.write_junit(summary["results"], str(base_dir / "junit.xml"), "qemu")


def machine_args(arch, iso, firmware=None, exit_device: bool = False) -> List[str]:
    """QEMU machine/media stanza per architecture; the single source, reused
    by plume's cmd_run so the two launchers cannot drift.

    riscv64 boots UEFI on virt: EDK2 code flash is read-only and the vars
    flash is snapshotted so parallel workers never contend on writes. The ISO
    must be a real SCSI CD-ROM -- a virtio-blk disk (what --cdrom degrades to
    on virt) never gets its El Torito boot image parsed. The guest-exit
    channel is virt's built-in sifive_test finisher, so no exit device is
    attached.
    """
    if arch == "riscv64":
        if not firmware:
            raise HarnessError("riscv64 requires --firmware (EDK2 RISCV_VIRT_CODE.fd)")
        vars_fd = str(firmware).replace("_CODE.fd", "_VARS.fd")
        return [
            "-M", "virt",
            "-drive", f"if=pflash,unit=0,format=raw,readonly=on,file={firmware}",
            "-drive", f"if=pflash,unit=1,format=raw,snapshot=on,file={vars_fd}",
            "-device", "virtio-scsi-pci,id=scsi0",
            "-drive", f"file={iso},format=raw,if=none,id=cd0,media=cdrom,read-only=on",
            "-device", "scsi-cd,drive=cd0,bus=scsi0.0",
        ]

    args = ["--cdrom", str(iso)]
    if exit_device:
        args.extend(["-device", "isa-debug-exit,iobase=0x604,iosize=0x02"])
    return args


# ----------------------------------------------------------------------
# Sharded, reboot-per-test execution (brute isolation)
# ----------------------------------------------------------------------
def _make_harness(args: argparse.Namespace) -> KernelHarness:
    memory = args.memory or (256 if args.arch == "riscv64" else 64)
    boot_timeout = args.boot_timeout or (60.0 if args.arch == "riscv64" else 30.0)
    qemu = args.qemu or f"qemu-system-{args.arch}"
    return KernelHarness(
        qemu=qemu, iso=args.iso, memory=memory,
        boot_timeout=boot_timeout, extra_args=args.qemu_arg,
        verbose=args.verbose, add_exit_device=not args.no_exit_device,
        arch=args.arch, firmware=args.firmware,
    )


def _infra_result(name: str, desc: Optional[TestDescriptor], reason: str) -> TestResult:
    return TestResult(
        name=name, outcome="infra",
        module=desc.module if desc else None,
        requires_clean_env=desc.requires_clean_env if desc else False,
        expects_crash=desc.expects_crash if desc else False,
        attempts=[AttemptLog(attempt=1, outcome="infra", reason=reason)],
    )


def _run_one_isolated(
    harness: KernelHarness, name: str, desc: Optional[TestDescriptor],
    args: argparse.Namespace, artifact_dir: Optional[Path],
) -> TestResult:
    """Brute isolation: boot a fresh kernel and run exactly one test. The whole boot+run is retried as
    a unit on a flaky boot or serial hiccup -- a decisive pass/fail/error outcome is never retried.
    This keeps a transient infrastructure failure (e.g. a slow boot under heavy parallelism) scoped to
    one test instead of letting it cascade."""
    result: Optional[TestResult] = None
    for _ in range(1 + max(0, args.retries)):
        try:
            harness.restart()
        except (HarnessError, HarnessProcessExit) as exc:
            result = _infra_result(name, desc, f"boot failed: {exc}")
            time.sleep(0.5)  # let transient spawn/resource contention clear before a fresh boot
            continue
        # The restart above is the clean boot, so run_test does a single attempt without restarting.
        result = harness.run_test(
            name, timeout=args.test_timeout, retries=0,
            requires_clean_env=False,
            expects_crash=desc.expects_crash if desc else False,
            module=desc.module if desc else None,
        )
        if result.outcome not in ("infra", "timeout"):
            break
    assert result is not None
    if result.attempts and result.attempts[-1].crash is not None:
        symbolicate_crash(result.attempts[-1].crash, args.kernel_elf)
    write_artifacts(artifact_dir, result)
    return result


def run_sharded(
    tests: List[str], descriptor_map: Dict[str, TestDescriptor],
    args: argparse.Namespace, artifact_dir: Optional[Path], jobs: int,
) -> List[TestResult]:
    """Each test runs in its own freshly booted kernel; `jobs` workers run independent QEMU
    processes in parallel. A boot or serial hiccup is isolated to one test, not a cascade."""
    work: "queue.Queue[str]" = queue.Queue()
    for name in tests:
        work.put(name)
    results: Dict[str, TestResult] = {}
    lock = threading.Lock()

    def worker() -> None:
        harness = _make_harness(args)
        try:
            while True:
                try:
                    name = work.get_nowait()
                except queue.Empty:
                    break
                desc = descriptor_map.get(name)
                try:
                    result = _run_one_isolated(harness, name, desc, args, artifact_dir)
                except (HarnessError, HarnessProcessExit) as exc:
                    result = _infra_result(name, desc, str(exc))
                    write_artifacts(artifact_dir, result)
                with lock:
                    results[name] = result
        finally:
            harness.stop()

    threads = [threading.Thread(target=worker, name=f"qemu-{i}", daemon=True) for i in range(jobs)]
    for i, th in enumerate(threads):
        th.start()
        if i + 1 < len(threads):
            time.sleep(0.15)  # stagger the initial QEMU spawns to avoid a thundering-herd boot
    for th in threads:
        th.join()
    return [results[name] for name in tests if name in results]


# ----------------------------------------------------------------------
# CLI
# ----------------------------------------------------------------------
def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Run Archipelago kernel tests under QEMU")
    p.add_argument("tests", nargs="*", help="Specific tests to run (defaults to all)")
    p.add_argument("--iso", default=None, type=Path, help="Path to the Archipelago ISO (default: build/<arch>/image.iso)")
    p.add_argument("--qemu", default=None, help="QEMU binary to use (default: qemu-system-<arch>)")
    p.add_argument("--arch", default="x86_64", choices=["x86_64", "riscv64"],
                   help="Guest architecture (selects machine model and exit-device handling)")
    p.add_argument("--firmware", type=Path, default=None,
                   help="UEFI firmware code image (required for riscv64; loaded as pflash)")
    p.add_argument("--memory", type=int, default=None,
                   help="Guest memory size in MiB (default: 64, or 256 on riscv64 where EDK2 needs the headroom)")
    p.add_argument("--boot-timeout", type=float, default=None,
                   help="Timeout for kernel boot (default: 30, or 60 on riscv64 to cover the EDK2 phase)")
    p.add_argument("--command-timeout", type=float, default=5.0, help="Timeout for list commands")
    p.add_argument("--test-timeout", type=float, default=5.0, help="Timeout for a test to finish")
    p.add_argument("--retries", type=int, default=3, help="Retries for infrastructure failures")
    p.add_argument("--jobs", "-j", type=int, default=0, help="Parallel QEMU workers (0 = auto: min(cpus, 8))")
    p.add_argument("--list", action="store_true", help="List tests and exit")
    p.add_argument("--artifacts", type=Path, default=None,
                   help="Artifact directory (default: build/<arch>/test-artifacts)")
    p.add_argument("--qemu-arg", action="append", default=[], help="Extra QEMU argument (repeatable)")
    p.add_argument("--no-artifacts", action="store_true", help="Disable artifact generation")
    p.add_argument("--no-exit-device", action="store_true", help="Don't attach isa-debug-exit")
    p.add_argument("--kernel-elf", type=Path, default=None,
                   help="Unstripped kernel ELF for crash symbolication (default: build/<arch>/obj/sys/kernel/kernel.elf)")
    p.add_argument("--verbose", action="store_true", help="Verbose harness logging")
    args = p.parse_args(argv)
    # Build output lives in a per-arch tree; derive the defaults once --arch is known.
    if args.iso is None:
        args.iso = Path(f"build/{args.arch}/image.iso")
    if args.kernel_elf is None:
        args.kernel_elf = Path(f"build/{args.arch}/obj/sys/kernel/kernel.elf")
    if args.artifacts is None:
        args.artifacts = Path(f"build/{args.arch}/test-artifacts")
    return args


def _discover(args: argparse.Namespace) -> List[TestDescriptor]:
    """Boot once to enumerate the registered tests, then shut that kernel down."""
    harness = _make_harness(args)
    try:
        harness.start()
        return harness.list_tests(args.command_timeout)
    finally:
        harness.stop()


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)

    if not args.iso.exists():
        print(f"[harness] error: ISO image not found: {args.iso}", file=sys.stderr)
        return 1

    artifact_dir: Optional[Path] = None if args.no_artifacts else args.artifacts
    if artifact_dir:
        if artifact_dir.exists():
            for item in artifact_dir.iterdir():
                shutil.rmtree(item) if item.is_dir() else item.unlink()
        artifact_dir.mkdir(parents=True, exist_ok=True)

    try:
        descriptors = _discover(args)
    except (HarnessError, HarnessProcessExit) as exc:
        print(f"[harness] error during discovery: {exc}", file=sys.stderr)
        return 1
    descriptor_map = {d.name: d for d in descriptors}

    if args.list and not args.tests:
        for d in descriptors:
            module = f" [{d.module}]" if d.module else ""
            print(f"{d.name}{module}")
        return 0

    if args.tests:
        tests_to_run = list(dict.fromkeys(args.tests))
        for name in tests_to_run:
            if name not in descriptor_map:
                print(f"[harness] warning: test '{name}' not discovered via LIST", file=sys.stderr)
    else:
        tests_to_run = [d.name for d in descriptors]
        if not tests_to_run:
            print("[harness] no tests discovered", file=sys.stderr)
            return 1

    jobs = args.jobs if args.jobs > 0 else min(os.cpu_count() or 1, 8)
    jobs = max(1, min(jobs, len(tests_to_run)))
    print(f"[harness] {len(tests_to_run)} test(s), reboot-per-test isolation, {jobs} parallel worker(s)\n")

    try:
        results = run_sharded(tests_to_run, descriptor_map, args, artifact_dir, jobs)
    except KeyboardInterrupt:
        print("[harness] interrupted", file=sys.stderr)
        return 130

    write_summary(artifact_dir, [_to_protocol_result(r) for r in results])

    failure_summaries: List[str] = []
    passed_count = failed_count = 0
    for result in results:
        if result.outcome in ("pass", "skipped"):
            passed_count += 1
            continue
        failed_count += 1
        attempt = result.attempts[-1] if result.attempts else None
        prefix = {"fail": "FAIL", "error": "ERROR", "timeout": "TIMEOUT", "infra": "INFRA"}.get(
            result.outcome, result.outcome.upper()
        )
        summary = f"{prefix}: {result.name} (attempts: {len(result.attempts)})"
        error_msgs: List[str] = []
        if attempt:
            error_msgs = [
                (e.payload.get("message") or e.payload.get("reason"))
                for e in attempt.events if (e.payload or {}).get("event") == "error"
            ]
            error_msgs = [m for m in error_msgs if m]
            if not error_msgs and attempt.reason:
                error_msgs.append(attempt.reason)
        if error_msgs:
            summary += "\n" + "\n".join(f"    - {msg}" for msg in error_msgs)
        failure_summaries.append(summary)

    for s in failure_summaries:
        print(s)
    if not failure_summaries:
        print("No test failures.")
    print(f"Summary: {passed_count} passed, {failed_count} failed")
    return 1 if failed_count > 0 else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
