# Testing

Archipelago has a kernel testing framework that runs tests inside QEMU and communicates results over the serial port.
A Python harness on the host drives the VM and collects results.

## Running Tests
```bash
make test                     # Build ISO and run all tests
make test TEST=<name>         # Run a single test
make test-verbose             # Verbose harness output
make test-verbose TEST=<name> # Verbose, single test
```

List available tests:

```bash
python3 tools/test-harness.py --list
```

Artifacts land in `build/<arch>/test-artifacts/<test-name>/`:

| File           | Contents                                             |
| -------------- | ---------------------------------------------------- |
| `console.log`  | Serial console output                                |
| `events.jsonl` | Structured harness events (one JSON object per line) |
| `harness.json` | Pass/fail metadata, timing, retries                  |

## Writing Tests
Tests live in `src/sys/kernel/tests/` (unit tests) or alongside the code they test (integration tests).

### Test Macros
From `kernel/testing/testing.h`:

```cpp
KTEST(name, module)                                // Standard test
```

Every QEMU-tier test runs in its own freshly booted kernel, so no macro-level isolation markers exist.

Most test files cover a single module, so the preferred style declares the module (and optional shared init) once and defines each test with `KTEST_CASE`:

```cpp
KTEST_MODULE_WITH_INIT("obj/object", obj_object_init);  // or KTEST_MODULE("obj/object");

static void obj_object_init() { register_all_test_types(); }

KTEST_CASE(obj_object_signal_set_and_clear) { ... }
KTEST_CASE_CRASH(...) { ... }  // expected to crash the kernel (crash = pass)
```

Prefer fewer tests with more assertions: one test per behavior story over one fixture, using the phase-reporting `EXPECT` forms below. Keep a test separate when it pins a specific past bug, needs a virgin VM, or crashes the kernel.

### Assertions
**REQUIRE** -- abort the test on failure:

```cpp
KTEST_REQUIRE(condition)
KTEST_REQUIRE_EQUAL(a, b)
```

**EXPECT** -- log failure but continue:

```cpp
KTEST_EXPECT(condition)
KTEST_EXPECT_EQUAL(a, b)
```

Use `REQUIRE` for preconditions that make the rest of the test meaningless.
Use `EXPECT` when you want to check multiple things and see all failures.

### Concurrency helpers
Freestanding tests that exercise the scheduler have two helpers. `KTEST_YIELD_UNTIL(cond)` yields until the condition holds (bounded, then `REQUIRE`s it), replacing hand-rolled spin loops. `kernel::testing::spawn_fn` (from `<kernel/testing/spawn.h>`) spawns a kernel thread running a capturing lambda held by reference on the caller's frame, so tests need no context struct or `void*` trampoline -- the test must wait for the thread before the lambda goes out of scope:

```cpp
volatile int phase = 0;
auto body          = [&] { phase = 1; };
KTEST_UNWRAP(t, kernel::testing::spawn_fn("worker", body));
KTEST_YIELD_UNTIL(phase == 1);
```

### Example
```cpp
#include <kernel/testing/testing.h>
KTEST(my_feature_test, my_module) {
    int x = compute_something();
    KTEST_REQUIRE(x > 0);
    KTEST_REQUIRE_EQUAL(x, 42);
}
```

### Registration
The macros place test descriptors in the `.ktests` linker section.
The shell's `test` command discovers them at boot by walking the section between `__start__ktests` and `__stop__ktests`.

## How It Works
When `CONFIG_KERNEL_TESTING` is enabled (the default), the [[Shell|kernel shell]] provides the `test` command group.
The shell boots into interactive mode; the host harness sends `harness enable` to switch to protocol mode, then drives tests programmatically.

### Protocol
The harness communicates with the shell over UART.
After connecting, it sends `harness enable` to enter protocol mode, then uses shell commands.

Commands (host to kernel):

| Command | Description |
|---------|-------------|
| `harness enable` | Switch to protocol mode |
| `test list` | Enumerate all registered tests |
| `test run <name>` | Run a specific test |

Events (kernel to host) are emitted as `@@HARNESS {...}` JSON lines:

| Event | Fields | Meaning |
|-------|--------|---------|
| `ready` | `protocol` | Shell is idle, ready for a command |
| `test` | `name`, `module` | Test descriptor (response to `test list`) |
| `test_start` | `name`, `timestamp` | Test beginning |
| `test_end` | `name`, `status`, `reason`, `timestamp` | Test completed (`pass`/`fail`) |
| `error` | `message` | Assertion failure or command error |
| `abort` | `code` | Guest requested QEMU exit |

### Harness Behavior
- Boots QEMU with `-serial stdio`, waits for boot output to settle, then sends `harness enable`.
- Waits for the `ready` event before sending commands.
- Retries infrastructure failures up to 3 times with a full VM restart.
- For integration tests, restarts QEMU before and after the test.

### Harness Options
| Flag | Default | Description |
|------|---------|-------------|
| `--iso` | `build/<arch>/image.iso` | Path to bootable image |
| `--memory` | 64 (x86_64), 256 (riscv64) | Guest RAM in MiB |
| `--boot-timeout` | 30s (x86_64), 60s (riscv64) | Wait for boot handshake |
| `--test-timeout` | 5s | Wait for test completion |
| `--retries` | 3 | Infrastructure failure retries |
| `--no-artifacts` | false | Skip artifact generation |
| `--list` | -- | List tests and exit |
| `--qemu-arg` | -- | Extra QEMU arguments (repeatable) |
