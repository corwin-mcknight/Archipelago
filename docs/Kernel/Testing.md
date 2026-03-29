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

Artifacts land in `build/test-artifacts/<test-name>/`:

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
KTEST_WITH_INIT(name, module, init_fn)             // With setup function
KTEST_INTEGRATION(name, module)                    // Requires a fresh VM
KTEST_WITH_INIT_INTEGRATION(name, module, init_fn) // Fresh VM + setup
```

Integration tests tell the harness to restart QEMU before and after the test, ensuring a clean environment.

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
The test runner discovers them at boot by walking the section between `__start__ktests` and `__stop__ktests`.

## How It Works
When `CONFIG_KERNEL_TESTING` is enabled (the default), the kernel enters the test runner at the end of the [[Boot Process]].
The runner and host harness (`tools/test-harness.py`) communicate over the [[Device Drivers|UART]].

### Protocol
Commands (host to kernel):

| Command | Description |
|---------|-------------|
| `LIST` | Enumerate all registered tests |
| `RUN <name>` | Run a specific test |

Events (kernel to host) are emitted as `@@HARNESS {...}` JSON lines:

| Event | Fields | Meaning |
|-------|--------|---------|
| `waiting` | `protocol` | Runner is idle, ready for a command |
| `test` | `name`, `module` | Test descriptor (response to `LIST`) |
| `test_start` | `name` | Test beginning |
| `test_end` | `name`, `status`, `reason` | Test completed (`pass`/`fail`/`error`/`skipped`) |
| `error` | `message`, `file`, `line` | Assertion failure |
| `abort` | `code` | Guest requested QEMU exit |

### Harness Behavior
- Boots QEMU with `-serial stdio`, waits up to 30s for the first `waiting` event.
- Retries infrastructure failures up to 3 times with a full VM restart.
- For integration tests, restarts QEMU before and after the test.

### Harness Options
| Flag | Default | Description |
|------|---------|-------------|
| `--iso` | `build/image.iso` | Path to bootable image |
| `--memory` | 64 | Guest RAM in MiB |
| `--boot-timeout` | 30s | Wait for boot handshake |
| `--test-timeout` | 5s | Wait for test completion |
| `--retries` | 3 | Infrastructure failure retries |
| `--no-artifacts` | false | Skip artifact generation |
| `--list` | -- | List tests and exit |
| `--qemu-arg` | -- | Extra QEMU arguments (repeatable) |
