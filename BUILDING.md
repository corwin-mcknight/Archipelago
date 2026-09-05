# Building Archipelago

Archipelago uses **Plume**, a Python-based package manager that orchestrates the build. Each component (kernel, bootloader, etc.) is a package that builds in isolation and installs to a per-target sysroot. The root Makefile is a thin wrapper around Plume.

## Prerequisites

The development container provides everything needed. For host builds, you need:

- LLVM 17+ (`clang`, `clang++`, `ld.lld`)
- NASM (x86_64 only)
- QEMU (`qemu-system-x86_64`; `qemu-system-riscv64` for the riscv64 target)
- xorriso
- mtools and curl (riscv64 only: UEFI boot filesystem and firmware download)
- Python 3.9+ with PyYAML
- GNU Make, Git

## Using the Development Container

1. **VS Code**: Open the project folder and accept the prompt to reopen in the container.

2. **Docker directly**:
   ```bash
   docker build -t archipelago-dev -f .devcontainer/Dockerfile .
   docker run -it --rm -v "$(pwd):/workspaces/archipelago" -w /workspaces/archipelago archipelago-dev
   ```

## Build Commands

```bash
make build                     # Build all packages (kernel + limine)
make install                   # Build + assemble bootable ISO
make test                      # Build, assemble ISO, run full test suite
make test TEST=<name>          # Run a single test by name
make test-verbose              # Tests with verbose harness output
make test-verbose TEST=<name>  # Run a single test by name
make uboot-test                # riscv64 boot-chain smoke test: U-Boot EFI -> Limine -> kernel in QEMU
make clean                     # Remove build artifacts (obj, sysroot, ISO)
make full-clean                # Remove entire build/ directory
make format                    # Run clang-format on kernel sources
make clangd                    # Regenerate compile_commands.json for IDE support
make docs                      # Generate Doxygen documentation
```

### Using Plume directly

For finer control, invoke Plume directly:

```bash
python3 -m plume build [package...]  # Build stale packages and compose the sysroot
python3 -m plume image               # Assemble the boot image (ISO or SD, per config) from sysroot
python3 -m plume test [test_name]    # Build + image + run tests
python3 -m plume list                # List all packages
python3 -m plume clean               # Remove build artifacts
python3 -m plume clangd              # Regenerate compile_commands.json
```

Every command accepts `--arch <arch>` to target an architecture for one invocation without touching the `default.yaml` selection, e.g. `python3 -m plume test --arch riscv64`.
`--arch all` runs the command once per target and prints a matrix summary, e.g. `python3 -m plume test --arch all`.

## How Plume Works

Plume reads package definitions from `repo/packages.yml`. Each package has a Makefile at `repo/packages/<category>/<name>/Makefile` that implements four stages:

| Stage | Purpose |
|-------|---------|
| `pkg_get_source` | Fetch or prepare source code |
| `pkg_configure` | Configure the build |
| `pkg_build` | Compile |
| `pkg_install` | Install outputs to a staging directory |

After all stages succeed, the staging directories are composed into the target's sysroot at `build/<arch>/sysroot/` -- the sysroot is rebuilt as the union of the staging trees whenever any package changes, never patched in place. The ISO is then assembled from the sysroot.

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
python3 -m plume set-config riscv64
```

Without a `default.yaml`, Plume falls back to `repo/config/x86_64.yaml`, and any single command can override the selection with `--arch`.
Everything downstream keys off the active config: the target triple and compiler flags, the boot artifacts installed into the sysroot, the ISO layout (the `image:` stanza), and the QEMU invocation used by `plume test` and `plume run`.
x86_64 builds a BIOS+UEFI hybrid ISO; riscv64 builds a UEFI-only ISO booted through EDK2 firmware on QEMU's `virt` machine, and the firmware is fetched automatically as a host tool during the build.
Each architecture builds in its own tree under `build/<arch>/` (host tools are shared at `build/tools/`), so targets never clobber each other and switching needs only `build` and `image` -- no clean.
Plume also stamps every build with a hash of the target config and the content of every input file: changing the toolchain, flags, or any source invalidates affected packages automatically, and mtime-only churn (branch switches) rebuilds nothing.
Packages gated to one architecture declare `arches:` in `repo/packages.yml` and are skipped elsewhere.

### Running Interactively

`make shell` boots the built ISO headless with the serial console on stdio; `make run` is the same with a display window (x86_64 only), and `make debug` adds a GDB stub that waits for attach.
All three build first and launch QEMU for the active target -- on riscv64 that includes the EDK2 firmware and SCSI CD-ROM plumbing automatically.

### Physical Boards

Board targets produce an SD-card image instead of an ISO (`image: format: sd` in the board config). For the JH7110 (Orange Pi RV, VisionFive 2):

```bash
python3 -m plume build --arch riscv64^jh7110
python3 -m plume image --arch riscv64^jh7110
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
