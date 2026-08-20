#!/usr/bin/env python3
"""Run kernel tests on the real board over the serial mux.

Speaks the same @@HARNESS protocol as tools/test-harness.py, but against the
netbooted Orange Pi RV instead of QEMU: each test gets a freshly rebooted
kernel (the fresh-kernel-per-test contract), driven via `reboot` at the shell,
or by waiting out the watchdog after a crash-expected test. Run `make netboot`
first so the board boots the build under test.

Usage:
  board_test.py --list
  board_test.py [--test NAME]... [--fresh]   (default: all tests)

Non-crash tests share one booted kernel (batched = speed); a test that crashes
or wedges the kernel is recorded as failed and the batch resumes on the next
boot. Crash-expected tests always get their own boot. --fresh reboots before
every test (fresh = truth) for bisecting state bleed between batch-mates.
"""

import argparse
import json
import pathlib
import socket
import sys
import time

HOST, PORT = "localhost", 5556
BOOT_MARKER = b"boot: initialization complete"
CRASH_MARKER = b"*** KERNEL CRASH ***"
BOOT_WAIT_S = 240  # covers netboot (~45s) and a watchdog reset (+60s)
TEST_WAIT_S = 120

LOG_PATH = pathlib.Path(__file__).resolve().parent.parent / "build/riscv64/jh7110/board-test.log"


class Board:
    def __init__(self):
        self.sock = socket.create_connection((HOST, PORT), timeout=5)
        self.sock.settimeout(0.3)
        self.buf = b""
        self.log = open(LOG_PATH, "ab")

    def send(self, line: str) -> None:
        # Pace the bytes: the shell drains the 16-byte RX FIFO from a 1 ms poll
        # loop and its echo can block on a busy TX FIFO, so full-line-rate input
        # overruns and silently drops the tail of a command.
        for b in line.encode() + b"\r":
            self.sock.sendall(bytes([b]))
            time.sleep(0.002)

    def pump(self) -> None:
        try:
            chunk = self.sock.recv(4096)
        except socket.timeout:
            return
        if not chunk:
            raise ConnectionError("serial mux closed the connection")
        self.buf += chunk
        self.log.write(chunk)
        self.log.flush()

    def wait_for(self, markers, timeout):
        """Pump until a marker appears on a live output line. Lines starting with
        @@ are protocol traffic -- a crash dump replays the boot log inside
        @@CRASH_LOG lines, which must not count as a real boot."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            flat = self.buf.replace(b"\r", b"")
            while (nl := flat.find(b"\n")) >= 0:
                line, flat = flat[:nl], flat[nl + 1:]
                if not line.startswith(b"@@") and any(m in line for m in markers):
                    self.buf = flat
                    return line
            self.buf = flat
            self.pump()
        return None

    def next_event(self, timeout):
        """Return the next @@HARNESS event as a dict, or None on timeout. Crashes surface as {'event': 'crash'}."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            flat = self.buf.replace(b"\r", b"")
            nl = flat.find(b"\n")
            if nl < 0:
                self.pump()
                continue
            line, self.buf = flat[:nl], flat[nl + 1:]
            # interactive-mode banner or harness-mode structured dump
            if CRASH_MARKER in line or line.startswith(b"@@CRASH_"):
                return {"event": "crash"}
            if line.startswith(b"@@HARNESS "):
                try:
                    return json.loads(line[len(b"@@HARNESS "):])
                except json.JSONDecodeError:
                    pass
        return None

    def fresh_shell(self, reboot: bool) -> None:
        """Get to a harness-mode shell on a freshly booted kernel."""
        if reboot:
            self.send("")
            self.send("reboot")
        if self.wait_for([BOOT_MARKER], BOOT_WAIT_S) is None:
            sys.exit("no kernel boot observed; is the board netbooted and the mux up? (make console to look)")
        time.sleep(0.3)
        # Input can vanish while the serial bridge re-syncs across a reset, so
        # resend the enable until a ready arrives, then drain the extra readys
        # the duplicate enables produced (mirrors tools/test-harness.py).
        for _ in range(4):
            self.send("harness enable")
            ev = self.next_event(5)
            while ev is not None:
                if ev.get("event") in ("ready", "waiting"):
                    while self.next_event(0.5) is not None:
                        pass
                    return
                ev = self.next_event(5)
        sys.exit("shell did not enter harness mode")

    def list_tests(self):
        self.send("test list")
        tests = []
        while True:
            ev = self.next_event(15)
            if ev is None or ev.get("event") in ("ready", "waiting"):
                return tests
            if ev.get("event") == "test":
                tests.append((ev["name"], bool(ev.get("expects_crash"))))

    def run_test(self, name: str, expects_crash: bool):
        """Returns (outcome, reason, crashed)."""
        self.send(f"test run {name}")
        outcome, reason, crashed = "infra", "no result events", False
        deadline = time.time() + TEST_WAIT_S
        while time.time() < deadline:
            ev = self.next_event(deadline - time.time())
            if ev is None:
                break
            kind = ev.get("event")
            if kind == "crash":
                outcome, reason, crashed = "crash", "kernel crashed", True
                break
            if kind == "error":
                reason = ev.get("message") or reason
            if kind == "test_end":
                outcome = "pass" if ev.get("status") == "pass" else "fail"
                reason = ev.get("reason") or reason
                break
        if expects_crash:  # crash is the passing outcome; surviving is the failure
            outcome = "pass" if outcome == "crash" else ("fail" if outcome == "pass" else outcome)
        elif outcome == "crash":
            outcome = "fail"
        return outcome, reason, crashed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test", action="append", help="test name to run (repeatable; default: all)")
    parser.add_argument("--list", action="store_true", help="list tests registered on the board")
    parser.add_argument("--fresh", action="store_true", help="reboot before every test instead of batching")
    args = parser.parse_args()

    board = Board()
    board.fresh_shell(reboot=True)
    tests = board.list_tests()
    if args.list:
        for name, expects_crash in tests:
            print(f"{name}{'  (crash)' if expects_crash else ''}")
        return 0

    known = dict(tests)
    if args.test:
        missing = [t for t in args.test if t not in known]
        if missing:
            sys.exit(f"unknown tests: {', '.join(missing)}")
        todo = [(t, known[t]) for t in args.test]
    else:
        todo = tests

    if not args.fresh:  # batch: non-crash tests share boots, crash tests boot alone at the end
        todo = [t for t in todo if not t[1]] + [t for t in todo if t[1]]

    results = []
    started = time.time()
    ready = True  # fresh harness shell from startup
    used = False  # whether that shell has run a test since its boot
    rebooting = False  # board is already rebooting on its own (post-crash)
    for i, (name, expects_crash) in enumerate(todo):
        item_start = time.time()
        if not ready or (used and (args.fresh or expects_crash)):
            board.fresh_shell(reboot=not rebooting)
            used = False
        outcome, reason, crashed = board.run_test(name, expects_crash)
        results.append((name, outcome, reason, crashed))
        used = True
        rebooting = crashed  # both expected and stray crashes reboot via harness_exit
        ready = not crashed and outcome != "infra"  # infra timeout: assume the kernel is suspect

        done = i + 1
        passed = sum(1 for r in results if r[1] == "pass")
        eta = (time.time() - started) / done * (len(todo) - done)
        status = f"[{done}/{len(todo)}] {outcome:<4}  {name}  {time.time() - item_start:.1f}s"
        status += f" | {passed} pass {done - passed} fail | eta {int(eta // 60)}:{int(eta % 60):02d}"
        print(status + (f"\n        reason: {reason}" if outcome != "pass" else ""), flush=True)

    # A batch-mode failure may be state bleed from sharing a boot, not a real
    # bug: retry each once on its own fresh kernel and believe that verdict.
    if not args.fresh:
        for i, (name, outcome, reason, crashed) in enumerate(results):
            if outcome == "pass":
                continue
            expects_crash = known[name]
            board.fresh_shell(reboot=not rebooting)
            outcome, reason, rebooting = board.run_test(name, expects_crash)
            if outcome == "pass":
                print(f"retry: {name} passes on a fresh boot -- state-dependent result; "
                      "suspect the test's virgin-kernel assumptions first, then the kernel", flush=True)
            else:
                print(f"retry: {name} still fails fresh ({reason})", flush=True)
            results[i] = (name, outcome, reason, rebooting)

    passed = sum(1 for r in results if r[1] == "pass")
    print(f"\nboard tier: {passed}/{len(results)} passed, {len(results) - passed} failed")
    print(f"raw serial log: {LOG_PATH}")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
