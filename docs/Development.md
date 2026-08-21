# Development
Archipelago is generally developed inside a devcontainer, which provides a consistent working environment. The devcontainer is not required, but simply provides a working out-of-the-box experience for compiling.
## Toolchain
The container is Alpine Linux with the following tools:

| Tool                     | Purpose                          |
| ------------------------ | -------------------------------- |
| `clang` / `clang++`      | C/C++ compiler                   |
| `ld.lld`                 | LLVM linker                      |
| `nasm`                   | x86 assembler                    |
| `qemu-system-x86_64`     | Emulator for running and testing |
| `xorriso`                | ISO image creation               |
| `lldb` / `gdb-multiarch` | Debuggers                        |
| `clang-extra-tools`      | clang-tidy, clangd, etc.         |
| `ripgrep`                | Fast code search                 |
| `python3` + `py3-yaml`   | Plume build system runtime       |
The `plume` shell alias is available for `python3 -m plume`.
## Building
```bash
make build              # Build all packages
make install            # Build + assemble bootable ISO
make test               # Build ISO and run test suite
make test TEST=<name>   # Run a single test
make clean              # Remove build artifacts
make format             # Run clang-format on kernel sources
make clangd             # Regenerate compile_commands.json
```

See [[Plume]] for the full command reference and build internals.
## IDE Setup
### clangd
Run `make clangd` to generate `compile_commands.json`. The clangd extension picks this up automatically for code navigation, completion, and diagnostics.
Regenerate after adding or renaming source files.
### Formatting
The project uses clang-format with a Google-derived style (4-space indent, 120-char lines). The `.clang-format` file in the repository root configures this. Format files with:

```bash
make format             # Format all kernel sources
clang-format -i <file>  # Format a specific file
```

### Debugging
`lldb` and `gdb-multiarch` are available in the container. QEMU can be started with GDB stub support, but note that `make run` and `make debug` should not be used from the CLI as they launch interactive QEMU. Use `make test` instead.

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
    testing/             Test runner
plume/                   Build system source (Python)
repo/
  config.yaml            Build configuration
  packages.yml           Package definitions
  packages/              Per-package Makefiles
  sets/                  Package sets (@system)

docs/                    Architecture, kernel, and development docs
  Design/                Planned architecture docs
  Kernel/                Current kernel docs and transition notes
tools/                   Test harness and scripts
```

## Persistent Volumes
The devcontainer mounts three named volumes so state survives container rebuilds:

| Volume                | Path                              | Contents                             |
| --------------------- | --------------------------------- | ------------------------------------ |
| `vscode-extensions`   | `/root/.vscode-server/extensions` | Installed VS Code extensions         |
| `vscode-server-data`  | `/root/.vscode-server/data`       | VS Code server state                 |
