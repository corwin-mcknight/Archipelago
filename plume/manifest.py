"""Package manifest generation, reading, and conflict detection."""

import hashlib
import json
import os

from plume.package import Package


def generate_manifest(package: Package, d_path: str) -> dict:
    """Walk the $D install tree and build a manifest dict.

    File paths are relative to $D (i.e. sysroot-relative).
    Each file entry includes its relative path, sha256, and size.
    """
    files = []
    for root, _dirs, filenames in os.walk(d_path):
        for fname in sorted(filenames):
            abs_path = os.path.join(root, fname)
            rel_path = os.path.relpath(abs_path, d_path)
            size = os.path.getsize(abs_path)
            sha = _sha256(abs_path)
            files.append({"path": rel_path, "sha256": sha, "size": size})

    files.sort(key=lambda f: f["path"])

    return {
        "package": package.full_name,
        "qualified_name": package.qualified_name,
        "category": package.category,
        "name": package.name,
        "version": package.version,
        "arch": package.arch,
        "dependencies": list(package.dependencies),
        "files": files,
    }


def write_manifest(manifest: dict, path: str):
    """Write a manifest dict as JSON."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")


def read_manifest(path: str) -> dict:
    """Read a manifest JSON file."""
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def installed_manifest_dir(sysroot: str) -> str:
    """Return the directory where installed manifests are stored."""
    return os.path.join(sysroot, "var", "plume", "manifests")


def manifest_filename(qualified_name: str) -> str:
    """Convert a qualified name to a manifest filename.

    'sys/kernel-0.0.1~x86_64' -> 'sys--kernel-0.0.1~x86_64.json'
    """
    return qualified_name.replace("/", "--") + ".json"


def installed_manifest_path(sysroot: str, qualified_name: str) -> str:
    """Return the full path of an installed manifest."""
    return os.path.join(installed_manifest_dir(sysroot), manifest_filename(qualified_name))


def save_installed_manifest(manifest: dict, sysroot: str):
    """Write a manifest to the sysroot manifests directory."""
    path = installed_manifest_path(sysroot, manifest["qualified_name"])
    write_manifest(manifest, path)


def list_installed_manifests(sysroot: str) -> list[dict]:
    """Read all installed manifests from the sysroot."""
    mdir = installed_manifest_dir(sysroot)
    if not os.path.isdir(mdir):
        return []
    manifests = []
    for fname in sorted(os.listdir(mdir)):
        if fname.endswith(".json"):
            manifests.append(read_manifest(os.path.join(mdir, fname)))
    return manifests


def check_conflicts(manifest: dict, sysroot: str, exclude_pkg: str | None = None) -> list[tuple[str, str]]:
    """Check if files in manifest conflict with other installed packages.

    Returns a list of (conflicting_file_path, owning_package_qualified_name).
    """
    new_files = {f["path"] for f in manifest["files"]}
    conflicts = []
    for installed in list_installed_manifests(sysroot):
        if installed["qualified_name"] == exclude_pkg:
            continue
        for f in installed["files"]:
            if f["path"] in new_files:
                conflicts.append((f["path"], installed["qualified_name"]))
    return conflicts


def _sha256(filepath: str) -> str:
    """Compute SHA-256 hex digest of a file."""
    h = hashlib.sha256()
    with open(filepath, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()
