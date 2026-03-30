# Kernel Shell

The kernel shell is an interactive command interface for developers, accessible over the UART serial port.
It provides diagnostics, testing, memory inspection, and boot flow control from a single prompt.

## Overview
After hardware initialization, the kernel enters the shell and displays a `% ` prompt.
The developer can run commands, inspect kernel state, and execute tests interactively.
The `boot continue` command resumes the normal boot sequence.

For CI and automated testing, the `harness enable` command switches the shell into protocol mode.
In protocol mode, output is emitted as machine-readable JSON events instead of human-readable text.
The Python test harness uses this mode to drive test execution programmatically.

## Modes
### Interactive Mode
The default mode.
Output is human-readable text.
A `% ` prompt is displayed before each command.
Backspace editing is supported.

### Protocol Mode
Activated by `harness enable`, deactivated by `harness disable`.
All output becomes `@@HARNESS` JSON lines with no prompt.
Protocol version 2 distinguishes it from the legacy test runner protocol.

## Commands
Commands use a subcommand structure -- the first word selects a command group, the rest are arguments.

| Group | Purpose |
|-------|---------|
| `test` | Run kernel tests (list, run, run-all) |
| `mem` | Inspect early heap and physical memory allocator |
| `handle` | Inspect the handle table |
| `obj` | Inspect the object type registry |
| `cpu` | Show processor state and uptime |
| `log` | View the kernel log buffer |
| `boot` | Resume the boot sequence |
| `harness` | Switch between interactive and protocol mode |
| `help` | List available commands |

## Command Registration
Command groups self-register using a linker section, following the same pattern as kernel tests.
Each group provides a name, description, and handler function.
The `KSHELL_COMMAND` macro places a descriptor in the `.kshell_cmds` section.
The shell discovers all registered commands at runtime by walking the section boundaries.

## Boot Integration
The shell is gated by `CONFIG_KERNEL_SHELL`.
When enabled, the kernel enters the shell after hardware initialization completes.
The shell runs until `boot continue` is issued, at which point it returns and the boot sequence proceeds.

Testing requires the shell -- `CONFIG_KERNEL_TESTING` cannot be enabled without `CONFIG_KERNEL_SHELL`.

## Shell and Testing
The shell's `test` command replaces the standalone test runner.
Test macros, assertions, and the `.ktests` linker section are unchanged.
See [[Testing]] for details on writing and running tests.
