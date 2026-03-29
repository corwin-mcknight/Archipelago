# Plume
Plume is Archipelago's package manager and build system. It builds packages from a single source tree, manages dependencies, and assembles bootable ISO images. The source lives in `plume/`.

## Quick Reference

```bash
make build              # Build all packages
make install            # Build, install to sysroot, assemble ISO
make test               # Build ISO and run the test suite
make test TEST=<name>   # Run a single test
make clean              # Remove build artifacts
make clangd             # Regenerate compile_commands.json
```

These Make targets wrap `python3 -m plume` commands.

## Packages

Packages are defined in `repo/packages.yml`. Each has a category, name, version, and optional dependencies.
### Example Packages
| Package                  | Description                               |
| ------------------------ | ----------------------------------------- |
| `boot/limine-tools-10.0` | Limine bootloader host tools (build tool) |
| `boot/limine-10.0`       | Limine boot binaries                      |
| `boot/limine-config-1.0` | Limine bootloader configuration           |
| `sys/kernel-0.0.1`       | The Archipelago kernel                    |
| `sys/kernel-src-0.0.1`   | Kernel source archive                     |

The `@system` package set (`repo/sets/system`) includes all packages needed for a bootable image.

### Package Structure
Each package has a Makefile at `repo/packages/<category>/<name>/Makefile` implementing four stages:

| Stage | Make Target | Purpose |
|-------|-------------|---------|
| 1 | `pkg_get_source` | Fetch or locate sources |
| 2 | `pkg_configure` | Configure the build |
| 3 | `pkg_build` | Compile |
| 4 | `pkg_install` | Install to staging directory |

### Live Sources
Packages can declare `supports_live_sources: true` with a `live_source_path` pointing into the source tree. Plume stamps the build and checks source modification times -- if any source file is newer than the stamp, the package rebuilds. The kernel and limine-config packages use this.

## Build Flow
### Dependency Resolution
Plume resolves dependencies via topological sort. Given a set of target packages, it expands all transitive dependencies and computes a build order where dependencies build first.

For `make install`:

```
1. boot/limine-tools-10.0   (build tool, no deps)
2. boot/limine-10.0         (depends on limine-tools)
3. boot/limine-config-1.0   (no deps)
4. sys/kernel-0.0.1          (depends on limine, limine-config)
5. sys/kernel-src-0.0.1      (no deps)
```

Steps that don't depend on each other can build in parallel with `-j`.

### Build Environment
Each package build receives environment variables:

| Variable | Value |
|----------|-------|
| `ARCH` | Target architecture (`x86_64`) |
| `SYSROOT` | `build/sysroot` |
| `WORKDIR` | `build/tmp/<category>/<name>-<version>` |
| `S` | Source directory (`$WORKDIR/src`) |
| `D` | Staging install directory (`$WORKDIR/install`) |
| `CC`, `CXX` | `clang`, `clang++` |
| `LD`, `AS` | `ld.lld`, `nasm` |
| `LIVE_SOURCES` | Source tree path (for live-source packages) |

### Sysroot
Packages install their files to a staging directory (`$D`), then Plume merges them into the shared sysroot at `build/sysroot/`. The sysroot becomes the root of the bootable ISO.

Installed package manifests (file paths, SHA256 checksums, metadata) are stored in `sysroot/var/plume/manifests/`. A world file at `sysroot/var/plume/world` tracks what is installed.

### Binary Packages
After a successful build, Plume caches a binary package (`.tar.xz`) in `build/packages/`. On subsequent installs, if the binary is cached and sources haven't changed, Plume extracts from the cache instead of rebuilding.

### ISO Assembly
`plume image` assembles the ISO:

1. **xorriso** creates a BIOS+UEFI bootable ISO from the sysroot
2. **limine bios-install** writes boot code to the ISO's MBR

The resulting image is `build/image.iso`.

## Build Output
```
build/
  obj/                     Intermediate build artifacts
  sysroot/                 Merged system root
    boot/                  kernel.elf, limine binaries, limine.conf
    var/plume/             Manifests and world file
  tools/                   Host build tools (limine binary)
  tmp/                     Per-package work directories
  packages/                Binary package cache (.tar.xz)
  compile_commands.json    For clangd
  image.iso                Bootable ISO
```

## Commands
All commands are invoked as `python3 -m plume <command>` or through the Makefile.

| Command | Description |
|---------|-------------|
| `build` | Build packages and their dependencies |
| `install` | Build and install to sysroot |
| `rebuild` | Clean and rebuild, optionally propagating to reverse dependencies |
| `uninstall` | Remove packages from sysroot |
| `image` | Assemble ISO from sysroot |
| `test` | Install, assemble ISO, run test harness |
| `status` | Show build and install state |
| `validate` | Check packages.yml and Makefiles |
| `clean` | Remove build artifacts |
| `list` | List packages with optional dependency tree |
| `world` | Show installed packages |
| `clangd` | Rebuild kernel with compile_commands.json generation |

## Configuration
Build settings live in `repo/config.yaml`:

| Setting | Value |
|---------|-------|
| `arch` | `x86_64` |
| `sysroot` | `./build/sysroot` |
| `cc` / `cxx` | `clang` / `clang++` |
| `ld` / `as` | `ld.lld` / `nasm` |
| `iso_output` | `./build/image.iso` |

Architecture-specific settings (QEMU binary, target triple) are also defined here under `arch_configs`.
