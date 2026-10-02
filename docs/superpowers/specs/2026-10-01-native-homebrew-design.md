# Native Homebrew Development
Archipelago's supported macOS workflow uses native Homebrew tools, a repository-local Python environment, and the existing Make/Plume commands. No container runtime, devcontainer CLI, or Linux VM participates in setup, builds, testing, indexing, or debugging. Linux command-line development remains supported.

## Toolchain and entry points
A Brewfile declares the native build dependencies. `make setup` installs missing tools and creates `.venv`; Python dependencies are declared and pinned. `tools/dev` establishes the project environment for a single command without modifying the user's shell configuration. Make targets use this environment automatically. `make doctor` checks the required tools and Python runtime without installing anything.

## Compilation and build state
Host executables use the native host ABI; kernel/userspace objects use their explicit freestanding target triples. Freestanding C++ compilation excludes the host C++ library headers. Package Makefiles honor configured compiler, linker, assembler, archive, and object-copy tools.

Plume includes host and toolchain identity in staleness and discards affected object files when build-affecting configuration or toolchain identity changes. Source-only changes retain Make's incremental compilation. Replacing a compiler at the same path or switching from an existing container build must not reuse incompatible host binaries or obsolete target objects.

## Hosted tests
Darwin uses Mach-O section registration and native linking; ELF hosts retain the existing linker fragment. Test discovery and expected-crash behavior remain equivalent. ASan/UBSan, coverage, libFuzzer, and TSan run locally. Reported peak RSS remains in KiB on both Darwin and Linux.

## Editors and debugging
Compilation database generation is a dry-run operation independent of a successful kernel link, emits commands for the selected target/board only, and uses current checkout paths. Local clangd discovers it through repository configuration. Worktrees retain independent output, target selection, and indexing.

QEMU retains its graphical x86_64 display and paused GDB-server mode. Document native symbol loading, source breakpoints, remote attach, and architecture-specific debugger support. Automated verification uses headless, bounded QEMU sessions only; interactive Make targets are never launched unattended.

## Completion criteria
Verify Plume regression checks, both freestanding target builds and boot images, all hosted tests, coverage, short fuzz runs, TSan, both headless QEMU test suites, and the RISC-V U-Boot smoke lane. Verify indexing from a separate checkout path and perform a bounded debugger attach smoke test where the available debugger supports the target. Record failures and limitations with evidence. Remove the devcontainer configuration from the maintained workflow and update authoritative documentation and the roadmap. Preserve unrelated files and leave reviewable local changes.
