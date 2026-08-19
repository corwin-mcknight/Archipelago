# Plume
Plume is Archipelago's build coordinator. It builds packages from a single source tree in dependency order, composes the resulting staging trees into a sysroot, and assembles bootable images from it. The source lives in `plume/`.

Plume is deliberately not a package manager: there is no installed-package database, no versions, and no uninstall. The sysroot is a derived artifact -- a pure function of the repository and the target config -- never a mutable system that state must be tracked against.

## Quick Reference

```bash
make build              # Build all packages and compose the sysroot
make install            # Build, compose, assemble the boot image
make test               # Build the image and run the test suite
make test TEST=<name>   # Run a single test
make clean              # Remove build artifacts
make clangd             # Regenerate compile_commands.json
```

These Make targets wrap `python3 -m plume` commands.

## Packages

Packages are defined in `repo/packages.yml`. Each is named `category/name` and declares a description and optional dependencies. There are no versions: everything builds from the tree at HEAD, and third-party packages pin their upstream commit in their own Makefile.
A package that only exists on some targets declares `arches:`; it is skipped everywhere else, and naming it explicitly on the wrong target is an error. `arches:` matches the bare architecture -- a board narrows a target, it never changes which architecture that target is.
A package that compiles board facts in declares `varies_by: ["board"]` and builds once per board; see Targets and Boards.
### Example Packages
| Package              | Description                               |
| -------------------- | ----------------------------------------- |
| `boot/limine-tools`  | Limine bootloader host tools (build tool) |
| `boot/limine`        | Limine boot binaries                      |
| `boot/limine-config` | Limine bootloader configuration           |
| `sys/kernel`         | The Archipelago kernel                    |
| `sys/kernel-headers` | Public kernel headers (user/kernel ABI)   |
| `sys/init`           | The first user program                    |

### Package Structure
Each package has a Makefile at `repo/packages/<category>/<name>/Makefile` implementing four stages:

| Stage | Make Target | Purpose |
|-------|-------------|---------|
| 1 | `pkg_get_source` | Fetch or locate sources |
| 2 | `pkg_configure` | Configure the build |
| 3 | `pkg_build` | Compile |
| 4 | `pkg_install` | Install to staging directory |

### Live Sources
Packages can declare `supports_live_sources: true` with a `live_source_path` pointing into the source tree. The kernel and limine-config packages use this; editing a watched source marks the package stale.

### Staleness
Every successful build writes a stamp recording a hash of the target config's build-affecting settings (architecture, toolchain, triple, flags) and a content hash of every input file -- the package's own files under `repo/packages/` for every package, plus the live source tree for live-source packages.
A package is stale exactly when that record differs from the present: a config change, or a source file whose content changed, appeared, or vanished. Modification times are never consulted, so a branch switch that restores identical content rebuilds nothing, and deleting a source file is a change like any other. Paths and run-only settings (QEMU, memory, image layout) are excluded from the config hash.

When a package rebuilds, Plume prints why on the package's status line -- a config change, or the changed file. `plume status` shows the same reason next to stale packages. Builds print one line per package; pass `--verbose` (`-v`) to stream per-stage output instead. Captured output from the most recent build of each package is kept at `build/<arch>/logs/<category>/<name>.log`, whether the build succeeded or failed.

## Build Flow
### Dependency Resolution
Plume resolves dependencies via topological sort and builds dependencies first; independent packages can build in parallel with `-j`.

`plume build` always operates on the whole system graph: every supported non-tool package, plus the build tools they depend on. Staleness checks make this cheap -- a fresh package costs one hash comparison. Naming packages on the command line scopes `--force` to them; naming only build tools (the host test lanes) builds just that closure and leaves the sysroot alone.

### Sysroot
Each package installs into its own staging directory (`$D`). The sysroot is the union of the system packages' staging trees, and it is composed, never patched: whenever any system package (re)builds or the composed set changes, Plume removes the sysroot and copies every staging tree back in, in dependency order. A package is copied in as soon as it is current, so a later package always compiles against its dependencies' installed files -- that is what lets `sys/init` build against the headers `sys/kernel-headers` installs to `/usr/include`, exactly as any other consumer of that ABI would.

Two packages installing the same path is an error caught during composition; there is no ownership database to consult because the staging trees themselves are the ownership record. A sibling stamp (`<sysroot>.stamp`) records what the sysroot was derived from, so an unchanged system recomposes nothing.

### Build Environment
Each package build receives environment variables:

| Variable | Value |
|----------|-------|
| `ARCH` | Target architecture (`x86_64`, `riscv64`), never board-qualified |
| `BOARD` | Board name; exported only to packages declaring `varies_by: ["board"]` |
| `TRIPLE` | Target triple from the config (e.g. `x86_64-unknown-none`) |
| `SYSROOT` | `build/<arch>/sysroot` |
| `WORKDIR` | `build/<arch>/tmp/<category>/<name>[^<board>]` |
| `OBJ_DIR` | `build/<arch>/obj/<category>/<name>[^<board>]` -- intermediate build tree |
| `S` | Source directory (`$WORKDIR/src`) |
| `D` | Staging install directory (`$WORKDIR/install`) |
| `CC`, `CXX` | `clang`, `clang++` |
| `LD`, `AS` | `ld.lld`, `nasm` |
| `LIVE_SOURCES` | Source tree path (for live-source packages) |

### Image Assembly
`plume image` assembles the boot image, driven by the config's `image:` stanza. `format` selects the layout; the default is `iso`:

1. **xorriso** creates a bootable ISO from the sysroot; `bios_boot` and `efi_boot` name the boot images
2. **limine bios-install** writes boot code to the ISO's MBR (only when `bios_boot` is configured)

`format: sd` instead builds an SD-card image for boards that boot through U-Boot's EFI loader: an MBR partition table with one FAT32 ESP holding the sysroot verbatim plus Limine's EFI executable at `/EFI/BOOT/`. It is built with mtools (no root privileges) and written to a card with `dd`.

The resulting image lands at the config's `image_output` path.

## Build Output
Each architecture builds in its own tree so targets never clobber each other; host tools are shared.

```
build/
  <arch>/                  Per-target tree (x86_64/, riscv64/)
    obj/                   Intermediate build artifacts
    sysroot/               Composed system root
      boot/                kernel.elf, limine binaries, limine.conf
    sysroot.stamp          What the sysroot was composed from
    tmp/                   Per-package work directories
    image.iso              Bootable ISO
  tools/                   Host build tools (limine, EDK2 firmware, test runners)
  compile_commands.json    For clangd (active target)
```

## Commands
All commands are invoked as `python3 -m plume <command>` or through the Makefile.
Every command accepts `--arch <target>` (or `--config <path>`) to select a target for one invocation without changing the `default.yaml` selection. A target is an arch (`riscv64`) or a board (`riscv64^jh7110`).
`--arch all` fans the command out over every arch at its default board; `--arch all-boards` covers every board target as well. Either ends with a per-target pass/fail summary; the exit code is nonzero if any target fails.

| Command | Description |
|---------|-------------|
| `build` | Build stale packages and compose the sysroot |
| `image` | Assemble the boot image from the sysroot |
| `test` | Build, compose, image, run the test harness |
| `uboot-test` | Boot the U-Boot EFI chain (SD image) in QEMU |
| `status` | Show build and sysroot state |
| `clean` | Remove build artifacts |
| `list` | List packages with optional dependency tree |
| `clangd` | Rebuild kernel with compile_commands.json generation |
| `set-config` | Select the active target config (symlinks `default.yaml`) |
| `run` | Launch the built ISO interactively in QEMU |
| `shell` | Open an interactive shell in a package's build environment |
| `deps` | Show a package's transitive and reverse dependencies, optionally as a tree |
| `log` | Print a package's most recent build log |

Package validation (names, dependency existence, cycles, board-config isolation) runs automatically at the start of every command.

## Targets and Boards
A target is an architecture, optionally narrowed to a board. `repo/config/riscv64.yaml` is an arch target; `repo/config/riscv64^jh7110.yaml` is a board target. Every arch config names its default board, so `--arch riscv64` already builds a board -- the qualifier is only spelled out when selecting a non-default one.

A board config names its arch with `base:` and declares only what differs. The base is merged underneath it, so toolchain settings live in one place and boards on an arch cannot drift apart:

```yaml
config:
  base: riscv64
  board: jh7110
  sysroot: ./build/riscv64/jh7110/sysroot
  image:
    format: sd
  image_output: ./build/riscv64/jh7110/sd.img
```

A board target must override `sysroot` and `image_output`, since it inherits the arch's build tree; validation rejects a board config that does not, and checks that the filename agrees with its `arch`/`board` keys.

Boards on one architecture share that architecture's build tree. A package that declares no board-specific behavior builds once and every board on the arch reuses it -- that is the normal case, not a cache miss. Only packages declaring `varies_by: ["board"]` build separately per board, and only those carry `^<board>` in their qualifier and workdir:

```
sys/kernel~riscv64^jh7110      board-varying: one build per board
boot/limine~riscv64            shared by every riscv64 board
```

Because of that sharing, `board` is excluded from the build hash: a board switch must not invalidate packages that do not depend on the board. Board-varying packages get their isolation from the qualifier in their paths instead.

`BOARD` is exported only to packages that declare the axis, so a package cannot read it without also getting the per-board workdir and cache entry that make that read correct. `ARCH` is always the bare architecture, never board-qualified.

`--arch all` runs every arch at its default board; `--arch all-boards` runs every board target as well.

## Configuration
Each target has a config at `repo/config/<target>.yaml`; the active one is the `default.yaml` symlink in the project root, managed with `plume set-config` (which accepts a bare target name, e.g. `plume set-config riscv64`).
When no selection exists, Plume falls back to `repo/config/x86_64.yaml`.

| Setting | Value |
|---------|-------|
| `arch` | `x86_64` or `riscv64` |
| `board` | Board this target builds for (default board on an arch config) |
| `base` | Arch config to merge underneath this one (board configs only) |
| `build_dir` / `sysroot` / `tmp_path` | Per-target tree under `./build/<arch>/` |
| `triple` | Target triple exported to package builds |
| `cc` / `cxx` | `clang` / `clang++` |
| `ld` / `as` | `ld.lld` / `nasm` |
| `image_output` | `./build/<arch>/image.iso` |
| `image` | Boot image layout (`format`: `iso`/`sd`; `efi_boot`, optional `bios_boot`) |
| `qemu` | QEMU binary for this target |
| `memory` | Guest memory for `plume run` in MiB |
| `firmware` | UEFI firmware image (riscv64 only) |
