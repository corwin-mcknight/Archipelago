# Initrd
Plume packages userspace in `boot/initrd.tar`, an uncompressed POSIX ustar archive. Limine supplies exactly two userspace modules: `boot/init.bin` tagged `init`, and the archive tagged `initrd`. The kernel loads the fixed-layout init image and sends init one read-only VMO over the archive with its exact byte length. The kernel does not interpret archive headers, member paths, or ELF executables.

## Archive format
Ustar keeps the initial reader small and lets ordinary tar tools inspect the image. Its fixed headers encode names, sizes, and checksums; Python's standard library writes the format without an additional build dependency. See the [ustar header reference](https://www.ibm.com/docs/en/zvm/7.4.0?topic=format-description-header-fields) and [Python tarfile documentation](https://docs.python.org/3/library/tarfile.html).

Archipelago uses a restricted profile:

- Regular files and directories only. Links, device nodes, sparse files, compression, GNU extensions, and pax extensions are unsupported.
- Canonical relative paths use printable ASCII, are at most 255 bytes, and contain no backslashes, empty components, `.` or `..`. A directory header may end in a slash; that slash is removed for comparison.
- Entries appear in strictly increasing order by canonical path. Duplicate names are rejected.
- Headers carry the `ustar` magic and version `00`, valid unsigned checksums, and ASCII octal numeric fields. File data is rounded to 512-byte blocks with zero padding. At least two zero blocks terminate the archive; any further padding is also zero.
- The complete archive is at most 64 MiB. Init validates the entire archive before starting a bootstrap executable, including headers and data bounds after any desired member.

Plume writes entries in canonical path order with timestamps and numeric owner IDs set to zero and empty owner names. Directories and executable files have mode `0755`; other files have mode `0644`. Identical content and executable bits therefore produce identical archives, independent of host ownership, timestamps, and directory traversal order.

## Bootstrap discovery
`bootstrap/<service>.elf` reserves the automatic startup namespace. Service names contain 1--31 characters, start with a lowercase ASCII letter, and thereafter use lowercase letters, digits, or underscores. The basename without `.elf` becomes the task name. `init` is reserved for the coordinator itself. Nested directories and other files within `bootstrap/` are rejected rather than silently skipped.

`bootstrap/elf_loader.elf` is required and always starts first, regardless of archive order. Init uses its userspace ELF library to construct that service and transfers its TaskFactory authority to it. Init then asks the loader to start the remaining bootstrap executables in archive order. The current coordinator supports at most eight ordinary bootstrap children in addition to the loader.

The current archive contains `bootstrap/elf_loader.elf`, `bootstrap/echo.elf`, and `bootstrap/selftest.elf`. Echo and selftest preserve the existing service integration check; selftest is a temporary automatically started test program. Future programs under `bin/` and data elsewhere in the archive are carried without being launched. The file server, shell, and ordinary program launch path remain separate work.

Init maps the original archive read-only and copies each selected executable into its own anonymous VMO. The loader receives a read-only handle with the executable starting at offset zero, preserving the existing ELF loading protocol. Init retains the original archive for the boot lifetime; handing it to the future file server for a read-only `/boot` tree remains to be implemented.

## Package inputs
Packages install archive members under `$D/usr/share/initrd/`. For example, installing `$D/usr/share/initrd/bootstrap/echo.elf` supplies `bootstrap/echo.elf` in the archive. This dedicated runtime tree keeps installed build headers and static libraries out of userspace boot storage.

Plume regenerates the archive from the composed runtime tree whenever it assembles an ISO or SD image, including the U-Boot smoke path. Package rebuilds replace their staging trees so removed outputs cannot remain as stale archive members. Boot images include the boot artifacts and Limine configuration; the SDK and loose runtime tree remain in the host sysroot. Netboot transfers init and the initrd alongside the kernel and bootloader.
