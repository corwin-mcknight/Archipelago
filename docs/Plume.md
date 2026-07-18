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
A package that only exists on some targets declares `arches:`; it is skipped everywhere else, and naming it explicitly on the wrong target is an error.
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

### Staleness
Every successful build writes a stamp recording a hash of the target config's build-affecting settings (architecture, toolchain, triple, flags).
A package is stale when its stamp is missing, when the recorded hash no longer matches the active config, or -- for live-source packages -- when a source file is newer than the stamp.
Editing a target config therefore rebuilds everything it affects; paths and run-only settings (QEMU, memory, image layout) are excluded from the hash.

When a package rebuilds, Plume prints why on the package's status line -- a config change, or the first source file found newer than the stamp. `plume status` shows the same reason next to stale packages. Builds print one line per package; pass `--verbose` (`-v`) to stream per-stage output instead. Captured output from the most recent build of each package is kept at `build/<arch>/logs/<category>/<name>.log`, whether the build succeeded or failed.

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
| `ARCH` | Target architecture (`x86_64`, `riscv64`) |
| `TRIPLE` | Target triple from the config (e.g. `x86_64-unknown-none`) |
| `SYSROOT` | `build/<arch>/sysroot` |
| `WORKDIR` | `build/<arch>/tmp/<category>/<name>-<version>` |
| `S` | Source directory (`$WORKDIR/src`) |
| `D` | Staging install directory (`$WORKDIR/install`) |
| `CC`, `CXX` | `clang`, `clang++` |
| `LD`, `AS` | `ld.lld`, `nasm` |
| `LIVE_SOURCES` | Source tree path (for live-source packages) |

### Sysroot
Packages install their files to a staging directory (`$D`), then Plume merges them into the target's sysroot at `build/<arch>/sysroot/`. The sysroot becomes the root of the bootable ISO.

Installed package manifests (file paths, SHA256 checksums, metadata) are stored in `sysroot/var/plume/manifests/`. A world file at `sysroot/var/plume/world` tracks what is installed.

### Binary Packages
After a successful build, Plume caches a binary package (`.tar.xz`) in `build/<arch>/packages/`. On subsequent installs, if the binary is cached and neither sources nor config have changed, Plume extracts from the cache instead of rebuilding.

### ISO Assembly
`plume image` assembles the ISO, driven by the config's `image:` stanza:

1. **xorriso** creates a bootable ISO from the sysroot; `bios_boot` and `efi_boot` name the boot images
2. **limine bios-install** writes boot code to the ISO's MBR (only when `bios_boot` is configured)

The resulting image is `build/<arch>/image.iso`.

## Build Output
Each architecture builds in its own tree so targets never clobber each other; host tools are shared.

```
build/
  <arch>/                  Per-target tree (x86_64/, riscv64/)
    obj/                   Intermediate build artifacts
    sysroot/               Merged system root
      boot/                kernel.elf, limine binaries, limine.conf
      var/plume/           Manifests and world file
    tmp/                   Per-package work directories
    packages/              Binary package cache (.tar.xz)
    image.iso              Bootable ISO
  tools/                   Host build tools (limine, EDK2 firmware, test runners)
  compile_commands.json    For clangd (active target)
```

## Commands
All commands are invoked as `python3 -m plume <command>` or through the Makefile.
Every command accepts `--arch <arch>` (or `--config <path>`) to target an architecture for one invocation without changing the `default.yaml` selection.
`--arch all` fans the command out over every config in `repo/config/` sequentially and ends with a per-target pass/fail summary; the exit code is nonzero if any target fails.

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
| `set-config` | Select the active target config (symlinks `default.yaml`) |
| `run` | Launch the built ISO interactively in QEMU |
| `shell` | Open an interactive shell in a package's build environment |
| `deps` | Show a package's transitive and reverse dependencies, optionally as a tree |
| `log` | Print a package's most recent build log |

## Configuration
Each architecture has a target config at `repo/config/<arch>.yaml`; the active one is the `default.yaml` symlink in the project root, managed with `plume set-config` (which accepts a bare arch name, e.g. `plume set-config riscv64`).
When no selection exists, Plume falls back to `repo/config/x86_64.yaml`.

| Setting | Value |
|---------|-------|
| `arch` | `x86_64` or `riscv64` |
| `build_dir` / `sysroot` / `tmp_path` | Per-target tree under `./build/<arch>/` |
| `triple` | Target triple exported to package builds |
| `cc` / `cxx` | `clang` / `clang++` |
| `ld` / `as` | `ld.lld` / `nasm` |
| `iso_output` | `./build/<arch>/image.iso` |
| `image` | Boot image layout (`efi_boot`, optional `bios_boot`) |
| `qemu` | QEMU binary for this target |
| `memory` | Guest memory for `plume run` in MiB |
| `firmware` | UEFI firmware image (riscv64 only) |
