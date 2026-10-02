"""Deterministic userspace archive built from package-owned runtime files."""

import os
from pathlib import Path
import re
import stat
import tarfile
import tempfile


RUNTIME_ROOT = Path("usr/share/initrd")
INITRD_PATH = Path("boot/initrd.tar")
MAX_ARCHIVE_SIZE = 64 * 1024 * 1024
MAX_PATH_SIZE = 255
MAX_BOOTSTRAP_SERVERS = 8  # Plus the dedicated ELF loader.
BOOTSTRAP_NAME = re.compile(r"bootstrap/([a-z][a-z0-9_]{0,30})\.elf\Z")


def _archive_name(path: str) -> str:
    """Match the small, portable pathname subset accepted by lib/initrd."""
    if (not path or len(path) > MAX_PATH_SIZE or path.startswith("/")
            or any(part in ("", ".", "..") for part in path.split("/"))
            or any(ord(c) < 32 or ord(c) > 126 or c == "\\" for c in path)):
        raise ValueError(f"invalid initrd path: {path!r}")
    return path


def write_archive(runtime_root, output):
    """Write sorted POSIX ustar, with only regular files and directories.

    Owner IDs, timestamps, owner names, and non-executable permission bits
    never depend on the host. Publish atomically so a bad input cannot leave
    a partially written archive available to an image assembler.
    """
    runtime_root, output = Path(runtime_root), Path(output)
    if not stat.S_ISDIR(runtime_root.lstat().st_mode):
        raise ValueError(f"initrd runtime root is not a directory: {runtime_root}")

    entries = []
    bootstrap_servers = 0

    def traversal_error(error):
        raise error

    for directory, dirs, files in os.walk(runtime_root, followlinks=False, onerror=traversal_error):
        for name in dirs + files:
            path = Path(directory) / name
            archive_name = _archive_name(path.relative_to(runtime_root).as_posix())
            mode = path.lstat().st_mode
            if not (stat.S_ISDIR(mode) or stat.S_ISREG(mode)):
                raise ValueError(f"unsupported initrd file type: {archive_name}")
            if archive_name == "bootstrap" and not stat.S_ISDIR(mode):
                raise ValueError("initrd bootstrap must be a directory")
            if archive_name.startswith("bootstrap/"):
                match = BOOTSTRAP_NAME.fullmatch(archive_name)
                if stat.S_ISDIR(mode) or match is None or match[1] == "init":
                    raise ValueError(f"invalid initrd bootstrap name: {archive_name}")
                if path.stat().st_size == 0:
                    raise ValueError(f"empty initrd bootstrap executable: {archive_name}")
                if match[1] != "elf_loader":
                    bootstrap_servers += 1
                    if bootstrap_servers > MAX_BOOTSTRAP_SERVERS:
                        raise ValueError("initrd has more than 8 bootstrap servers")
            entries.append((archive_name, path, mode))

    output.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".initrd-", dir=output.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            with tarfile.open(fileobj=stream, mode="w", format=tarfile.USTAR_FORMAT, encoding="ascii") as archive:
                for name, path, mode in sorted(entries):
                    info = tarfile.TarInfo(name)
                    info.mode = 0o755 if stat.S_ISDIR(mode) or mode & 0o111 else 0o644
                    if stat.S_ISDIR(mode):
                        info.type = tarfile.DIRTYPE
                        archive.addfile(info)
                    else:
                        info.size = path.stat().st_size
                        if info.size > MAX_ARCHIVE_SIZE:
                            raise ValueError(f"initrd input exceeds 64 MiB: {name}")
                        with path.open("rb") as payload:
                            archive.addfile(info, payload)
                    if stream.tell() > MAX_ARCHIVE_SIZE:
                        raise ValueError("initrd archive exceeds 64 MiB")
            if stream.tell() > MAX_ARCHIVE_SIZE:
                raise ValueError("initrd archive exceeds 64 MiB")
        os.replace(temporary, output)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def assemble_initrd(sysroot):
    """Refresh boot/initrd.tar from the current composed runtime tree."""
    sysroot = Path(sysroot)
    for required in (sysroot / "boot/init.bin", sysroot / RUNTIME_ROOT / "bootstrap/elf_loader.elf"):
        if not required.is_file() or required.is_symlink():
            raise ValueError(f"{required} missing or not a regular file; run `plume build`")
    write_archive(sysroot / RUNTIME_ROOT, sysroot / INITRD_PATH)
