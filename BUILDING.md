# Building Archipelago

Archipelago uses **Plume**, a Python-based package manager that orchestrates the build. Each component (kernel, bootloader, etc.) is a package that builds in isolation and installs to a shared sysroot. The root Makefile is a thin wrapper around Plume.

## Prerequisites

The development container provides everything needed. For host builds, you need:

- LLVM 17+ (`clang`, `clang++`, `ld.lld`)
- NASM
- QEMU (`qemu-system-x86_64`)
- xorriso
- Python 3.9+ with PyYAML
- GNU Make, Git

## Using the Development Container

1. **VS Code**: Open the project folder and accept the prompt to reopen in the container.

2. **Docker directly**:
   ```bash
   docker build -t archipelago-dev -f .devcontainer/Dockerfile .
   docker run -it --rm -v $(pwd):/workspaces/archipelago archipelago-dev
   ```

## Build Commands

```bash
make build                   # Build all packages (kernel + limine)
make install                 # Build + assemble bootable ISO
make test                    # Build, assemble ISO, run full test suite
make test TEST=<name>        # Run a single test by name
make test-verbose            # Tests with verbose harness output
make clean                   # Remove build artifacts (obj, sysroot, ISO)
make full-clean              # Remove entire build/ directory
make format                  # Run clang-format on kernel sources
make clangd                  # Regenerate compile_commands.json for IDE support
make docs                    # Generate Doxygen documentation
```

### Using Plume directly

For finer control, invoke Plume directly:

```bash
python3 -m plume build [package...]  # Build specific or all packages
python3 -m plume image               # Assemble ISO from sysroot
python3 -m plume test [test_name]    # Build + image + run tests
python3 -m plume list                # List all packages
python3 -m plume clean               # Remove build artifacts
python3 -m plume clangd              # Regenerate compile_commands.json
```

## How Plume Works

Plume reads package definitions from `repo/packages.yml`. Each package has a Makefile at `repo/packages/<category>/<name>/Makefile` that implements four stages:

| Stage | Purpose |
|-------|---------|
| `pkg_get_source` | Fetch or prepare source code |
| `pkg_configure` | Configure the build |
| `pkg_build` | Compile |
| `pkg_install` | Install outputs to a staging directory |

After all stages succeed, the staging directory is merged into the shared sysroot at `build/sysroot/`. The ISO is then assembled from the sysroot.

### Current packages

| Package | Description |
|---------|-------------|
| `boot/limine-10.0` | Limine bootloader (cloned from GitHub, build tool) |
| `sys/kernel-0.0.1` | Archipelago kernel (built from `src/sys/kernel/`) |

### Configuration

- `repo/config.yaml` — architecture, paths, toolchain settings
- `repo/packages.yml` — package manifest with dependencies

## Build Output

```
build/
  obj/sys/kernel/        # Kernel object files and kernel.elf
  sysroot/               # Assembled system root (kernel + boot files)
  tools/limine/          # Limine bootloader binaries
  tmp/                   # Per-package working directories
  image.iso              # Bootable ISO image
  compile_commands.json  # For clangd/IDE support
```
