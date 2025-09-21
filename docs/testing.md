# Kernel Testing Framework

This document describes the kernel test mode that currently ships in Archipelago and the Python harness that drives it. The final section captures the key improvements we would still like to pursue.

## Quick Start
1. Build an ISO with testing enabled (the default config already defines `CONFIG_KERNEL_TESTING = 1`):
   ```bash
   make install
   ```
2. List the tests discovered by the kernel:
   ```bash
   python3 tools/test-harness.py --list
   ```
3. Run the full suite (or pass one or more test names):
   ```bash
   python3 tools/test-harness.py               # run everything
   python3 tools/test-harness.py factorial_test
   ```
4. Inspect run artifacts under `build/test-artifacts/<test-name>/`. Each directory contains the raw console log (`console.log`) and the structured event stream (`events.jsonl`).

## Kernel Test Mode
The x86_64 entry point (`src/sys/kernel/x86_64/main.cpp`) jumps straight into the test runner when `CONFIG_KERNEL_TESTING` is set. The runner lives in `src/sys/kernel/x86_64/test_runner.cpp` and drives the in-kernel registry exposed by `kernel/testing/testing.h`.

### Test Registration
- Define tests with `KTEST(name, module)` or `KTEST_WITH_INIT(name, module, init_fn)`.
- Use `KTEST_INTEGRATION(name, module)` or `KTEST_WITH_INIT_INTEGRATION(name, module, init_fn)` when the test must start from a pristine VM; these mark the test descriptor with `requires_clean_env = true`.
- Tests live in the `.ktests` linker section. At boot the runner walks the descriptors between `__start__ktests` and `__stop__ktests`.
- `KTEST_REQUIRE` and `KTEST_REQUIRE_EQUAL` abort the current test and surface a `fail` result. They emit an `@@HARNESS {"event":"error"}` line before forcing the abort path so failures are easy to spot in the log.
- `kernel::testing::abort(code)` ends the current test, emits an `abort` event, and requests QEMU termination through the ISA debug exit device.

### Command Protocol
Communication happens over the primary UART. The runner reads carriage-return terminated commands and mirrors them back to the console for legibility. The protocol version is currently `1`.

Supported commands:
- `LIST` — emits one descriptor per test: `@@HARNESS {"event":"test","name":"<id>","module":"<module>"}`.
  - Integration tests add `"requires_clean_env":true` to that payload.
- `RUN <test_name>` — runs the named test and emits:
  - `@@HARNESS {"event":"test_start","name":"<id>"}`
  - zero or more `@@HARNESS {"event":"error", ...}` lines if assertions fire
  - `@@HARNESS {"event":"test_end","name":"<id>","status":"pass|fail|error|skipped","reason":"..."}`
  - `@@HARNESS {"event":"abort","code":<value>}` if the guest requested an exit (e.g., `kernel::testing::abort`)
- Any other command responds with `@@HARNESS {"event":"error","message":"Unknown command: ..."}`.

Between commands the runner stays in a waiting loop and repeatedly emits `@@HARNESS {"event":"waiting","protocol":1}` so the harness knows when it can dispatch the next instruction.

## Host Harness (`tools/test-harness.py`)
The harness launches QEMU, streams the serial console, and speaks the protocol above.

### Lifecycle
- Boot: `KernelHarness.start()` spawns QEMU with `-serial stdio`, waits up to `--boot-timeout` seconds (default 30) for the first `waiting` event, and records the process handle.
- Listing tests: `LIST` results are merged from individual `test` events and optional batched `list` payloads (not yet emitted by the kernel but accepted for forward compatibility).
- Running tests: the harness sends `RUN <name>` and waits for the next `waiting` marker. It summarises the stream into one of `pass`, `fail`, `error`, `timeout`, or `infra` (infrastructure failure). Infrastructure outcomes trigger up to `--retries` additional attempts (default 3) with a full VM restart in between.
- Test ordering aims to exercise each module in turn while deferring clean-environment tests until the end to minimise restarts where possible.
- When the descriptor advertises `requires_clean_env`, the harness restarts QEMU before the first attempt and again after the test finishes so subsequent runs always start clean. The request is captured in the run metadata.
- Shutdown: the harness always tears QEMU down when it exits or on Ctrl+C.

### CLI Highlights
- `--iso` (default `build/image.iso`) — path to the bootable image.
- `--memory` (default 64) — guest memory size in MiB.
- `--boot-timeout` / `--command-timeout` (defaults 30s / 5s) — waiting windows for the boot handshake and for `LIST` responses.
- `--test-timeout` (default 5s) — window to observe `test_end` after issuing `RUN`.
- `--retries` (default 3) — number of times to retry infrastructure failures with a fresh VM.
- `--list` — show available tests and exit.
- `--no-artifacts` — disable artifact generation; otherwise logs live under `build/test-artifacts/`.
- `--qemu-arg` — supply extra QEMU arguments (repeatable).
- `--no-exit-device` — run without attaching the ISA debug exit helper.

### Artifact Format
For each test attempt the harness writes:
- `console.log`: every line observed on the serial console for the final attempt.
- `events.jsonl`: structured events (only the JSON payload, one object per line) for the final attempt.
- `harness.json`: metadata describing duration, retries, clean-environment requests, and final status.

## Current Limitations and Future Improvements
The implementation is intentionally minimal; the items below remain on the roadmap:
- Expand the protocol with a dedicated `@@TEST` channel so structured test telemetry is separated from harness control flow.
- Emit richer metadata (timings, metrics, crash dumps) and include it in artifacts.
- Add explicit subtest helpers and reporting so a single top-level test can expose nested results.
- Surface watchdog heartbeats and more granular status updates while a long-running test executes.
- Parallelise execution across multiple VMs or cores once the kernel supports independent instances.
- Allow the harness to request a clean reboot between tests instead of always reusing the same kernel session.

These improvements can build on the current foundation without breaking the existing interface.
