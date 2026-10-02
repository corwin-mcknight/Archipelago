# Development
Archipelago uses a **native local toolchain** on macOS, with Homebrew dependencies and a checkout-local Python environment. Linux command-line development is also supported. [BUILDING.md](../BUILDING.md) is authoritative for setup, target selection, build commands, and output paths.

Planning and coordination live in [GitHub issues](https://github.com/corwin-mcknight/Archipelago/issues), [milestones](https://github.com/corwin-mcknight/Archipelago/milestones), and the [project](https://github.com/users/corwin-mcknight/projects/1). Follow [CONTRIBUTING.md](../CONTRIBUTING.md) for scope, dependencies, review, and validation evidence.

## Getting started
From the checkout root, run `make setup`, `make doctor`, and `make selfcheck`, then build or test with the normal Make targets. Setup reads `Brewfile` on macOS and installs the pinned dependencies from `requirements.txt` into `.venv`. Python 3.10 or newer is required.

```bash
make build                      # Build packages and compose the sysroot
make install                    # Build and assemble the boot image
make host-test TEST=<name>      # One architecture-independent hosted test
make test TEST=<name>           # One freestanding/QEMU test
make test ARCH=riscv64          # Select a target for this invocation
make clangd ARCH=x86_64         # Generate indexing commands without compiling
```

**`tools/dev <command>`** selects the checkout's Python runtime and native tools without changing the caller's working directory or shell configuration. Normal Make targets already use this environment. Use the wrapper for direct commands, for example `tools/dev python3 -m plume status`, `tools/dev clang-format -i <file>`, or `tools/dev clangd`.

Hosted tests build for the native macOS or Linux ABI; kernel and userspace packages cross-compile using their freestanding target triples. The compiler, linker, archive, object-copy, and assembler tools come from the configured build environment. macOS host lanes cover ASan/UBSan (`make host-test`), LLVM coverage (`make host-coverage`), libFuzzer (`make host-fuzz FUZZ=demangle FUZZ_TIME=10`), and TSan (`make host-tsan`).

## Editors, AI tools, and worktrees
### clangd
Run `make clangd` to create `build/compile_commands.json` for the selected architecture/board. Generation uses the kernel Makefile's dry-run commands, so it needs no successful compile or link. The repository's `.clangd` finds the database. Configure any editor's language server launcher as `tools/dev clangd`; use the wrapper's absolute path when the editor does not start from the checkout root. The database contains absolute compiler and source paths for that checkout.

Regenerate after adding or renaming sources, switching the selected target, or creating a worktree. `make clangd ARCH=riscv64` selects RISC-V for indexing without changing `default.yaml`. A checkout has one active database at a time.

Local AI coding tools can work directly on the ordinary source tree. Git worktrees need no editor integration: run `make setup` inside each worktree and keep its `.venv`, target selection, build output, and compilation database local. Do not share an object tree between worktrees. A new checkout path needs a newly generated database so navigation follows its own sources. On a fresh worktree, `make test ARCH=riscv64` prepares managed EDK2 firmware automatically; attended run/debug commands do the same, and `make uboot-test` prepares its U-Boot tool. No separate manual firmware build is required.

### Optional VS Code support
The checked-in VS Code tasks run native Make commands. Install the clangd extension and start a fresh VS Code process through `tools/dev code .` from the checkout root so the language server inherits Homebrew's tool paths. If VS Code is already running, quit it first; an existing process retains its previous environment. The settings select `clangd` from that environment and disable competing C/C++ IntelliSense. VS Code is optional; the same indexing and debugging commands work from other editors or a terminal.

### Formatting
The project uses clang-format with a Google-derived style: four-space indentation and a 120-character line limit. The root `.clang-format` configures it.

```bash
make format                        # Format all kernel sources
tools/dev clang-format -i <file>   # Format one file
```

## Debugging
### QEMU display and pause-on-start
In an attended terminal, use `make run ARCH=x86_64` to open the native QEMU window (Cocoa on macOS), or `make shell` for a headless serial console. `make debug ARCH=x86_64` builds the image and starts the graphical guest **paused**, with its GDB stub listening on TCP port **1234**. The serial log stays in the launching terminal. RISC-V interactive runs use the serial console and EDK2 firmware rather than the x86 display path.

Plume passes `-gdb tcp:127.0.0.1:1234 -S`: the stub listens only on loopback, and guest execution waits until the debugger resumes it. See the official [QEMU GDB guide](https://www.qemu.org/docs/master/system/gdb.html) and [display options](https://www.qemu.org/docs/master/system/invocation.html). Unattended verification uses `make test` or `make uboot-test`; do not launch `make run`, `make shell`, or `make debug` unattended.

### Native LLDB attachment
`make setup` installs Homebrew's standalone LLDB, and `tools/dev lldb` selects it. The x86_64 sequence below was verified with **Homebrew LLDB 23.1.2** in a bounded headless QEMU session. Load the unstripped ELF for the same image you started, select the remote platform, and connect to QEMU's stub:

```text
(lldb) platform select remote-gdb-server
(lldb) target create --arch x86_64 build/x86_64/sysroot/boot/kernel.elf
(lldb) gdb-remote 127.0.0.1:1234
(lldb) breakpoint set --hardware --name _start
(lldb) continue
```

The initial stop is in firmware, before the kernel's high-half mappings exist. A hardware breakpoint at `_start` lets execution reach the kernel without trying to patch unmapped kernel memory. Once stopped in the kernel, inspect or step through source:

```text
(lldb) source list
(lldb) register read rip
(lldb) image lookup -n _start
(lldb) thread backtrace
(lldb) breakpoint set --file main.cpp --line 149
(lldb) next
(lldb) continue
```

The verified smoke attached, hit the hardware `_start` breakpoint, decoded RIP, resolved symbols to `x86_64/main.cpp`, and printed a backtrace. Cocoa is available in the installed QEMU; opening a graphical QEMU window or editor debugger was not part of unattended verification.

Choose a current line in `src/sys/kernel/x86_64/main.cpp` rather than relying on the example's line number after edits. For a source file with the same basename on several targets, use its full checkout path. LLDB's [remote-debugging documentation](https://lldb.llvm.org/use/remote.html) describes the protocol and target setup. An editor debugger that supports LLDB remote commands can use the same symbol file and connection; it must attach rather than launch the ELF as a macOS program.

### RISC-V attachment
**Homebrew LLDB 23.1.2 also passed a bounded RISC-V attach smoke.** Start `make debug ARCH=riscv64` in an attended terminal and load that image's symbols:

```text
(lldb) platform select remote-gdb-server
(lldb) target create --arch riscv64 build/riscv64/sysroot/boot/kernel.elf
(lldb) gdb-remote 127.0.0.1:1234
(lldb) breakpoint set --hardware --name _start
(lldb) continue
(lldb) register read pc
(lldb) image lookup -n _start
(lldb) thread backtrace
```

The smoke verified remote attachment, the hardware breakpoint, PC decoding, a backtrace, and source resolution to `riscv64/main.cpp`. These checks used headless QEMU; source stepping, an editor debugger, and a graphical window were not tested. Board targets require the ELF from their own sysroot and separate hardware/debugger validation.

Keep debugger compatibility checks separate from compiler support. LLDB's [platform support page](https://lldb.llvm.org/) lists RISC-V as active development, so recheck attachment and register/breakpoint behavior when upgrading. A native GDB built with RISC-V support is an optional alternative: load the same ELF with `file`, run `target remote 127.0.0.1:1234`, set `hbreak _start`, and `continue`.

## Code Style
- 4 spaces, no tabs. 120-character line limit.
- `snake_case` for variables, functions, files, namespaces.
- `CamelCase` for classes and structs.
- `UPPER_SNAKE_CASE` for constants, macros, enum values.
- No exceptions, no RTTI, no standard library -- use [[KTL]] equivalents.
- Strings are opaque UTF-8 byte sequences. Never reject or strip bytes >= 0x80, never apply per-byte transformations that assume ASCII (case folding, character classes), and never split a multibyte sequence when truncating or editing. Only code that measures or edits text needs codepoint awareness; use the `<ktl/utf8>` helpers. Codepoints approximate display columns -- double-cell characters (CJK, emoji) are out of scope.
- Comment only non-obvious behavior. The code should be self-documenting; a comment earns its place by explaining hidden intent, an edge case, a mathematical constraint, or an API requirement, in one tight line. Never write a comment that justifies an edit or a symbol's existence -- that belongs in the commit message.
- Use `nullptr`, `constexpr`, `const` appropriately.
- Prefer composition over inheritance.

The `.clang-format` and `.editorconfig` files in the repository root enforce these conventions.

## Project Layout
```
src/sys/kernel/          Kernel source
  core/                  Boot, logging, panic, time, interrupts, and freestanding runtime support
    drivers/             Core drivers (for example, the shared 16550 UART implementation)
    std/                 Freestanding libc replacements
  task/                  Task lifecycle, scheduling, and synchronization primitives
  obj/                   Kernel objects and handle management
  crash/                 Crash reporting, symbols, and demangling
  elf/                   ELF parsing and loading
  syscalls/              System-call dispatch
  mm/                    Memory management (early heap, PMM)
  tests/                 Unit tests
  includes/
    kernel/              Kernel headers
    ktl/                 Kernel Template Library headers
    std/                 Standard library replacement headers
  x86_64/                Architecture-specific code
    platforms/pc/        PC board facts (PIT calibration reference, debug exit)
    tests/               Architecture tests
  riscv64/               Architecture-specific code
    platforms/virt/      QEMU virt board support
    platforms/jh7110/    JH7110 board support
    tests/               Architecture tests
  boot/limine/           Limine boot-protocol support
plume/                   Build system source (Python)
repo/
  config/                Architecture and board target configurations
  packages.yml           Package definitions
  packages/              Per-package Makefiles
  sets/                  Package sets (@system)

docs/                    Architecture, kernel, and development docs
  Design/                Planned architecture docs
  Kernel/                Current kernel docs and transition notes
tools/                   Test harness and scripts
```
