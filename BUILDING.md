# Building Archipelago

Archipelago uses **Plume**, a Python-based package manager that orchestrates the build. Each component (kernel, bootloader, etc.) is a package that builds in isolation and installs to a per-target sysroot. The root Makefile is a thin wrapper around Plume.

## Native macOS setup
The supported macOS workflow uses **Homebrew tools** and a repository-local Python environment. Install Homebrew and Apple's Command Line Tools (`xcode-select --install`) first, then run from the checkout root:

```bash
make setup       # Install Brewfile prerequisites and pinned Python dependencies in .venv
make doctor      # Check the environment without installing or building
make selfcheck   # Run Plume's host regression checks
```

`Brewfile` declares LLVM, LLD, LLDB, NASM, QEMU, image tools, GNU Make, Python, and development utilities. Setup uses Homebrew's native installation on Apple Silicon or Intel; it does not require changes to your shell startup files. Python must be **3.10 or newer**. The local `.venv` supplies the Python dependencies pinned in `requirements.txt`, including PyYAML.

**`tools/dev <command>`** selects the checkout's Python environment and Homebrew toolchain for one process. Make targets use it automatically. It preserves the caller's working directory, so run project commands from the checkout root; use it explicitly for direct Python, compiler, debugger, and editor commands. Homebrew LLVM takes precedence over Apple's compiler, while hosted tools build for the Mac and freestanding packages use explicit target triples. No Docker, Linux VM, or devcontainer CLI is needed.

### Linux prerequisites
Linux command-line development uses the same Make and Plume entry points. Install LLVM 17+ (`clang`, `clang++`, `ld.lld`, `llvm-ar`, `llvm-objcopy`, `clangd`), NASM, QEMU for x86_64 and riscv64, xorriso, mtools, curl, xz, coreutils (`sha256sum` and `truncate`), `ar` and `tar` for U-Boot archive extraction, Python 3.10+, GNU Make, and Git using your distribution's package manager. `make setup` creates the local Python environment; `make doctor` checks the result. Doxygen is needed for `make docs`, and socat is needed for the physical-board serial console.

## Build Commands

```bash
make build                     # Build all packages (kernel, userspace, bootloader)
make install                   # Build + assemble bootable ISO
make test                      # Build, assemble ISO, run full test suite
make test TEST=<name>          # Run a single test by name
make test-verbose              # Tests with verbose harness output
make test-verbose TEST=<name>  # Run a single test by name
make uboot-test                # riscv64 boot-chain smoke test: U-Boot EFI -> Limine -> kernel in QEMU
make clean                     # Remove build artifacts (obj, sysroot, ISO)
make full-clean                # Remove entire build/ directory
make format                    # Run clang-format on kernel sources
make clangd                    # Generate selected-target compile_commands.json without compiling
make docs                      # Generate Doxygen documentation
```

### Using Plume directly

For finer control, invoke Plume directly:

```bash
tools/dev python3 -m plume build [package...]  # Build stale packages and compose the sysroot
tools/dev python3 -m plume image              # Assemble the boot image (ISO or SD, per config) from sysroot
tools/dev python3 -m plume test [test_name]    # Build + image + run tests
tools/dev python3 -m plume list               # List all packages
tools/dev python3 -m plume clean              # Remove build artifacts
tools/dev python3 -m plume clangd              # Regenerate compile_commands.json
tools/dev python3 -m plume.selfcheck           # Host checks for Plume and deterministic initrd packaging
```

Make targets accept `ARCH=<target>` (for example, `make test ARCH=riscv64`). Every Plume command accepts `--arch <arch>` to target an architecture for one invocation without touching the `default.yaml` selection, e.g. `tools/dev python3 -m plume test --arch riscv64`.
`--arch all` runs the command once per target and prints a matrix summary, e.g. `tools/dev python3 -m plume test --arch all`.

## Hosted tests
Architecture-independent tests run as native host executables; QEMU tests exercise freestanding and target-specific behavior. The macOS host lanes include ASan/UBSan, LLVM coverage, libFuzzer, and ThreadSanitizer:

```bash
make host-test                      # Hosted suite with ASan/UBSan
make host-test TEST=<name>          # One hosted test
make host-coverage                  # Coverage report and configured gate
make host-fuzz FUZZ=demangle FUZZ_TIME=10  # Bounded fuzz run
make host-tsan                      # Threaded stress tests under TSan
```

Hosted artifacts remain under `build/tools/`; reports and fuzz corpora remain under `build/`. See [Kernel Testing](docs/Kernel/Testing.md) for the test protocol and available lanes.

## Continuous integration
[GitHub Actions CI](.github/workflows/ci.yml) defines independent checks for pull requests, pushes to `main`, and manual dispatch. Jobs use native tools through the shared [setup action](.github/actions/setup-native/action.yml), with Ubuntu 24.04 x64 and macOS 15 arm64 runners.

| Check category | Runner and scope |
| --- | --- |
| **Hosted tests** | Ubuntu and macOS: Plume/environment selfchecks, ASan/UBSan suite, registration/RSS and coverage-failure regressions |
| **Hosted coverage** | Ubuntu: full hosted line coverage with **`COV_MIN=85`** |
| **QEMU tests** | Ubuntu x86_64/pc and riscv64/virt: boot images, freestanding tests, target indexing; RISC-V also runs the U-Boot boot-chain smoke |
| **JH7110 build and SD image** | Ubuntu: board-target compilation and image assembly |
| **ThreadSanitizer and bounded fuzzing** | macOS: both TSan harnesses and all six fuzz targets, with five seconds per target |

The JH7110 job validates compilation and packaging; **physical-board testing remains separate**. The Ubuntu coverage job enforces the 85% threshold and retains its measured coverage report with each run.

Reproduce hosted checks with the commands above and the focused scripts `tools/dev python3 tools/host-portability-check.py` and `tools/dev python3 tools/host-coverage-check.py`. The coverage gate is `make host-coverage COV_MIN=85`. CI passes explicit QEMU controls through Make and Plume:

```bash
make test ARCH=riscv64 QEMU_JOBS=2 QEMU_BOOT_TIMEOUT=120 \
  QEMU_COMMAND_TIMEOUT=15 QEMU_TEST_TIMEOUT=30 QEMU_RETRIES=1
make uboot-test
make install 'ARCH=riscv64^jh7110'
```

Use `ARCH=x86_64` for the other QEMU lane. **`QEMU_JOBS` controls guest workers**, independently of Plume's `--jobs` package-build concurrency. Timeout values are seconds; retries cover infrastructure/timeouts rather than assertion failures. Omitted controls retain the local harness defaults. Both `make test` and `make test-verbose` accept these variables; direct Plume commands expose corresponding `--qemu-*` options. Additional checks use `make host-tsan` and `make host-fuzz FUZZ=<target> FUZZ_TIME=5` for `demangle`, `fmt`, `json`, `elf`, `elf-loader`, and `rbtree`.

Jobs retain artifacts for **14 days**: native setup/build/test logs under `build/ci/`, hosted and QEMU JUnit/JSON/console/crash results, kernel symbols and compilation databases, coverage summaries/profiles, fuzz corpora/reproducers, and boot/SD images. Result/log uploads run even when a preceding check fails so available diagnostics remain reviewable. Check the workflow run's artifact list for each runner or target.

**Activation requires committing and pushing the workflow to GitHub with Actions enabled.** Manual dispatch requires the workflow on the default branch. The local configuration and command checks do not establish a GitHub runner result; verify the first Actions run after pushing. Required status checks and branch protection are separate repository settings.

## How Plume Works

Plume reads package definitions from `repo/packages.yml`. Each package has a Makefile at `repo/packages/<category>/<name>/Makefile` that implements four stages:

| Stage | Purpose |
|-------|---------|
| `pkg_get_source` | Fetch or prepare source code |
| `pkg_configure` | Configure the build |
| `pkg_build` | Compile |
| `pkg_install` | Install outputs to a staging directory |

After all stages succeed, the staging directories are composed into the target's sysroot at `build/<arch>/sysroot/` -- the sysroot is rebuilt as the union of the staging trees whenever any package changes, never patched in place. The ISO is then assembled from the sysroot.

Userspace packages install runtime content under `usr/share/initrd/` in the sysroot. Image assembly packs it into a deterministic uncompressed ustar archive at `boot/initrd.tar`; Limine loads only `boot/init.bin` and that archive as userspace modules. Bootstrap servers live at `bootstrap/<service>.elf` inside the archive; ordinary executables and data can use other directories. Both ISO and SD images contain the boot artifacts, while headers, libraries, and the loose runtime tree remain in the host sysroot. See `docs/Plume.md` and `docs/Design/Initrd.md` for the packaging and naming contract.

### Packages

Package definitions live in `repo/packages.yml`. See `docs/Plume.md` for the full list and details on the package format.

### Configuration

- `repo/config/<arch>.yaml` -- one target config per architecture (paths, toolchain, QEMU, firmware)
- `default.yaml` -- symlink in the project root pointing at the selected target config
- `repo/packages.yml` -- package manifest with dependencies

### Target Architecture

Each architecture has its own target config under `repo/config/` (`x86_64.yaml`, `riscv64.yaml`).
Board targets narrow an architecture to real hardware and are named `<arch>^<board>` (`riscv64^jh7110.yaml` for the StarFive JH7110 in the Orange Pi RV and VisionFive 2); they inherit the arch config via `base:` and override only the board and its output paths.
The active target is the `default.yaml` symlink in the project root, managed with `plume set-config`:

```bash
tools/dev python3 -m plume set-config riscv64
```

Without a `default.yaml`, Plume falls back to `repo/config/x86_64.yaml`, and any single command can override the selection with `--arch`.
Everything downstream keys off the active config: the target triple and compiler flags, the boot artifacts installed into the sysroot, the ISO layout (the `image:` stanza), and the QEMU invocation used by `plume test` and `plume run`.
x86_64 builds a BIOS+UEFI hybrid ISO; riscv64 builds a UEFI-only ISO booted through EDK2 firmware on QEMU's `virt` machine. **Runtime commands prepare their own managed host tools:** `make test ARCH=riscv64` and attended `make run`, `make shell`, or `make debug` with `ARCH=riscv64` fetch/build EDK2 when needed. `make uboot-test` prepares U-Boot for its separate boot chain. Ordinary builds and image assembly omit unused runtime tools and hosted test lanes.

The RISC-V EDK2 launch uses **`virt,acpi=off`**, so Limine and the kernel use QEMU's device tree for hardware discovery. QEMU 8.2's ACPI hart table lacks the MMU capability entries required by the pinned Limine release; its device tree supplies them correctly. This keeps the same four-hart boot path working on Ubuntu's QEMU 8.2 and newer native QEMU releases.
Each architecture builds in its own tree under `build/<arch>/` (host tools are shared at `build/tools/`), so targets never clobber each other and switching needs only `build` and `image` -- no clean.
Plume stamps each build with build-affecting target settings, host/platform and resolved toolchain identity, and input-file content. Compiler upgrades, native/host changes, flag changes (including coverage), and source changes invalidate affected packages automatically. Build-environment changes discard incompatible objects; source-only changes retain incremental compilation. Modification-time churn alone rebuilds nothing.
Fresh checkouts and worktrees need no manual firmware-package build before these Make commands. A direct `tools/dev python3 -m plume run --arch riscv64` also prepares managed firmware but expects an already assembled image. Dry runs do not fetch or build runtime tools. A `firmware:` path outside the managed EDK2 location is user-supplied and remains your responsibility.

Packages gated to one architecture declare `arches:` in `repo/packages.yml` and are skipped elsewhere.

### Running Interactively

`make shell` boots the built ISO headless with the serial console on stdio. `make run` opens the x86_64 display window; native macOS QEMU provides a Cocoa display. `make debug` starts QEMU paused with its GDB stub on `127.0.0.1:1234`. These are interactive commands for an attended terminal; unattended checks use `make test` or `make uboot-test`. See [Development](docs/Development.md#debugging) for symbols, source breakpoints, and debugger attachment.
All three build first and launch QEMU for the active target -- on riscv64 that includes the EDK2 firmware and SCSI CD-ROM plumbing automatically.

### Physical Boards

Board targets produce an SD-card image instead of an ISO (`image: format: sd` in the board config). For the JH7110 (Orange Pi RV, VisionFive 2):

```bash
tools/dev python3 -m plume build --arch riscv64^jh7110
tools/dev python3 -m plume image --arch riscv64^jh7110
# then write build/riscv64/jh7110/sd.img to a card (destructive -- check the device name):
#   dd if=build/riscv64/jh7110/sd.img of=/dev/<sdcard> bs=4M conv=fsync
```

The board boots via its SPI-flash U-Boot, whose EFI loader finds Limine on the card's FAT partition; the console is UART0 at 115200 baud. `make uboot-test` rehearses the same boot chain headlessly in QEMU against the virt target's sysroot, using mainline U-Boot fetched as a host tool -- run it before blaming the board.
Day-to-day work on the board uses netboot instead of reflashing the card: `make netboot`, `make console`, and `make board-test` are described in `docs/Kernel/JH7110 Board.md`.

## Build Output

```
build/
  <arch>/                # Per-target build tree (x86_64/, riscv64/)
    obj/sys/kernel^<board>/ # Kernel object files and kernel.elf (pc, virt, jh7110)
    sysroot/             # Assembled system root (kernel + boot files)
    tmp/                 # Per-package working directories
    image.iso            # Bootable ISO image
  tools/                 # Shared host tools (limine, EDK2 firmware, test runners)
  compile_commands.json  # For clangd/IDE support (active target)
```
