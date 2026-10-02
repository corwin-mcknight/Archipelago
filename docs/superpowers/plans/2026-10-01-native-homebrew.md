# Native Homebrew Implementation Plan
> **For agentic workers:** Use the parallel-agent workflow for independent file scopes, with integration and a fresh review before completion. Steps use checkboxes for tracking.

**Goal:** Build, test, index, and debug Archipelago natively on macOS using Homebrew.

**Architecture:** Preserve Plume and its target configurations. Provision native tools separately from target compilation, port hosted-test registration and linking, and make build state sensitive to the executing host and toolchain.

**Tech Stack:** Homebrew, LLVM/LLD, GNU Make, Python/PyYAML, QEMU, NASM, xorriso, mtools.

**Spec:** `docs/superpowers/specs/2026-10-01-native-homebrew-design.md`

## Global Constraints
- No Docker, VM, or devcontainer CLI in the supported workflow.
- Preserve Linux command-line development and explicit x86_64/riscv64 target selection.
- Preserve unrelated working-tree files. Commit and publish only with explicit user authorization, on a feature branch rather than `main`.
- Use failing behavior checks before portability and build-state fixes.
- Do not launch interactive QEMU targets unattended.

## Review Focus
- Upgraded compilers at unchanged command paths must invalidate objects and host tools.
- A failed build after invalidation must not recover a stale success stamp.
- Indexing must exclude another target's or board's fragments.
- Darwin test sections must stay contiguous under ASan and retain expected-crash semantics.
- Setup and command wrappers must select the checkout's own Python environment from another working directory.

## Task 1: Native environment
**Files:** Brewfile, requirements files, tools/setup, tools/dev, tools/doctor.py, Makefile, .gitignore, .clangd.
**Interfaces:** `tools/dev <command> [arguments...]` executes with native tools and checkout-local `.venv` on PATH; `make setup`, `make doctor`, and `make selfcheck` are supported.
- [x] Reproduce missing dependencies and incorrect tool selection.
- [x] Implement idempotent setup and environment wrapper with clear failures.
- [x] Test wrapper selection, doctor, and Make commands in the configured environment.

## Task 2: Darwin hosted lanes
**Files:** testing.h, host_runner.cpp, hosted test package Makefiles, focused portability check.
**Interfaces:** Existing host-test/coverage/fuzz/TSan commands retain their protocol and package names.
- [x] Reproduce Mach-O section failure with a real hosted test.
- [x] Implement Darwin section boundaries/native linking and normalize RSS.
- [x] Run all hosted tests, coverage, bounded fuzzing, and TSan.

## Task 3: Plume and target portability
**Files:** plume toolchain/stamp/builder/cli/env code, target Makefiles, compile database tooling, regression checks.
**Interfaces:** Existing Plume commands and target configs remain supported; clangd emits selected-target commands without compilation.
- [x] Add failing regressions for toolchain changes, object invalidation, compiler overrides, and selected-target indexing.
- [x] Implement host/toolchain fingerprints and safe object invalidation.
- [x] Honor configured tools and exclude host C++ headers in target compilation.
- [x] Generate compile commands through a Make dry run; verify from an alternate checkout path.
- [x] Run regression checks and both full target builds/images.

## Task 4: Integration and debugging
**Files:** BUILDING.md, docs/Development.md, docs/Plume.md, README.md, todo.md, devcontainer configuration.
- [x] Install missing Homebrew/Python prerequisites using the new setup path.
- [x] Verify all hosted and headless target lanes, plus RISC-V U-Boot smoke.
- [x] Verify debugger attachment without opening an unattended interactive QEMU session.
- [x] Document native setup, per-worktree use, clangd, graphical QEMU, debugger attachment, and verified boundaries.
- [x] Remove the obsolete devcontainer configuration and have an independent agent review the complete diff.
