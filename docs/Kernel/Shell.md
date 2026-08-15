# Kernel Shell

The kernel shell is an interactive command interface for developers, accessible over the UART serial port.
It provides diagnostics, testing, memory inspection, and boot flow control from a single prompt.

## Overview
After hardware initialization, the kernel enters the shell and displays a `% ` prompt.
The developer can run commands, inspect kernel state, and execute tests interactively.
The `boot continue` command resumes the normal boot sequence and exits the shell; `boot shell` resumes it with the prompt kept live alongside.

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
| `mem` | Memory debug view: physical memory, page states, heap, kernel address space, VMOs |
| `handle` | Inspect the handle table |
| `obj` | Inspect the object type registry |
| `log` | View the kernel log buffer |
| `boot` | Resume the boot sequence |
| `harness` | Switch between interactive and protocol mode |
| `help` | List available commands |

## Memory Debug View
The `mem` command prints a plain, line-oriented memory summary: physical allocator totals, page descriptor states, early-heap statistics, and kernel address-space bounds and faults. Its output is identical in interactive and protocol modes, so harness tests can match stable field names without terminal escape sequences.

## Command Registration
Command groups self-register using a linker section, following the same pattern as kernel tests.
Each group provides a name, description, and handler function.
The `KSHELL_COMMAND` macro places a descriptor in the `.kshell_cmds` section.
The shell discovers all registered commands at runtime by walking the section boundaries.

## Boot Integration
The shell is gated by `CONFIG_KERNEL_SHELL`, and the kernel command line selects one of three boot modes:

| Token | Mode |
|-------|------|
| `shell` | Shell only -- the boot sequence holds until a `boot` command resumes it |
| `shell+boot` | Shell and boot -- the boot sequence proceeds with the prompt live alongside it |
| (none) | Boot only -- no shell |

A plain boot is the absence of a request, not a token; a shell request the build cannot honor degrades to a plain boot with a warning.
Continuing the boot sequence means launching the coordinator, and it happens exactly once no matter which path requests it: `boot continue` exits the shell as it does so, `boot shell` keeps the prompt, and either after the first is answered with "already continued".

Testing requires the shell -- `CONFIG_KERNEL_TESTING` cannot be enabled without `CONFIG_KERNEL_SHELL`.

## Shell and Testing
The shell's `test` command replaces the standalone test runner.
Test macros, assertions, and the `.ktests` linker section are unchanged.
See [[Testing]] for details on writing and running tests.
