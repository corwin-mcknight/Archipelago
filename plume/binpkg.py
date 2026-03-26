"""Binary package creation and extraction.

A binary package is a .tar.xz archive containing:
    manifest.json       — package metadata and file list
    data/               — the install tree (sysroot-relative paths)
"""

import io
import json
import os
import tarfile

from plume.config import Config
from plume.package import Package


BINPKG_EXT = ".tar.xz"


def _binpkg_dir(config: Config) -> str:
    """Return the binary package cache directory."""
    return os.path.join(config.get("build_dir"), "packages")


def binpkg_path(config: Config, package: Package) -> str:
    """Return the full cache path for a binary package.

    Layout: {build_dir}/packages/{category}/{name}-{version}~{arch}.tar.xz
    """
    return os.path.join(
        _binpkg_dir(config),
        package.category,
        f"{package.name}-{package.version}~{package.arch}{BINPKG_EXT}",
    )


def binpkg_exists(config: Config, package: Package) -> bool:
    """Check if a cached binary package exists."""
    return os.path.isfile(binpkg_path(config, package))


def create_binpkg(config: Config, package: Package, d_path: str, manifest: dict) -> str:
    """Create a binary package from the $D tree and manifest.

    Returns the archive path.
    """
    archive_path = binpkg_path(config, package)
    os.makedirs(os.path.dirname(archive_path), exist_ok=True)

    with tarfile.open(archive_path, "w:xz") as tar:
        # Write manifest.json at archive root
        manifest_bytes = json.dumps(manifest, indent=2).encode("utf-8") + b"\n"
        info = tarfile.TarInfo(name="manifest.json")
        info.size = len(manifest_bytes)
        tar.addfile(info, io.BytesIO(manifest_bytes))

        # Add all files from $D under data/ prefix
        for root, _dirs, files in os.walk(d_path):
            for fname in sorted(files):
                abs_path = os.path.join(root, fname)
                rel_path = os.path.relpath(abs_path, d_path)
                arcname = os.path.join("data", rel_path)
                tar.add(abs_path, arcname=arcname)

    return archive_path


def extract_binpkg(archive_path: str, sysroot: str) -> dict:
    """Extract a binary package into the sysroot.

    Extracts data/ contents into sysroot and returns the manifest dict.
    """
    with tarfile.open(archive_path, "r:xz") as tar:
        # Read manifest
        mf = tar.extractfile("manifest.json")
        if mf is None:
            raise ValueError(f"No manifest.json in {archive_path}")
        manifest = json.loads(mf.read())

        # Extract data/ members into sysroot
        data_prefix = "data/"
        for member in tar.getmembers():
            if not member.name.startswith(data_prefix):
                continue
            rel_path = member.name[len(data_prefix):]
            if not rel_path:
                continue
            # Security: reject absolute paths and traversal
            if rel_path.startswith("/") or ".." in rel_path.split(os.sep):
                continue

            dst_path = os.path.join(sysroot, rel_path)
            if member.isdir():
                os.makedirs(dst_path, exist_ok=True)
            elif member.isfile():
                os.makedirs(os.path.dirname(dst_path), exist_ok=True)
                src = tar.extractfile(member)
                if src is not None:
                    with open(dst_path, "wb") as dst:
                        while True:
                            chunk = src.read(65536)
                            if not chunk:
                                break
                            dst.write(chunk)
                    # Preserve permissions
                    os.chmod(dst_path, member.mode)

    return manifest
