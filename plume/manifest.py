"""Package manifest generation, reading, and conflict detection."""

import hashlib
import json
import os
import sys

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


def installed_manifest_path(sysroot: str, qualified_name: str) -> str:
    """Return the full path of an installed manifest."""
    filename = qualified_name.replace("/", "--") + ".json"
    return os.path.join(installed_manifest_dir(sysroot), filename)


def _identity(manifest: dict) -> tuple:
    """Package identity ignoring version and target qualifier.

    Two manifests with the same identity are the same package, so the newer
    install supersedes the older -- whether it differs by version bump or by
    gaining a board qualifier. Matches World's category/name keying.

    Returns None when either component is missing: a partially written or
    foreign manifest must never compare equal to another one, because
    identity drives file deletion.
    """
    category, name = manifest.get("category"), manifest.get("name")
    return (category, name) if category and name else None


def remove_manifest_files(manifest: dict, sysroot: str, keep: frozenset = frozenset()) -> int:
    """Delete the files a manifest owns, then prune the directories left empty.

    Paths in *keep* are skipped -- used when a newer install of the same
    package already owns them. Returns the number of files removed.
    """
    removed_dirs: set[str] = set()
    count = 0
    for entry in manifest.get("files", []):
        if entry["path"] in keep:
            continue
        fpath = os.path.join(sysroot, entry["path"])
        if os.path.isfile(fpath):
            os.remove(fpath)
            removed_dirs.add(os.path.dirname(fpath))
            count += 1

    for d in sorted(removed_dirs, key=len, reverse=True):
        while d != sysroot and os.path.isdir(d) and not os.listdir(d):
            os.rmdir(d)
            d = os.path.dirname(d)
    return count


def save_installed_manifest(manifest: dict, sysroot: str):
    """Write a manifest to the sysroot manifests directory.

    A previous install of the same package under a different qualified name
    (version bump, or newly board-varying) is superseded: any file it owned
    that the new install does not is removed, then its manifest is dropped.
    Deleting the record alone would strand those files owned by nobody --
    unattributable to `check_conflicts` and unreachable by `uninstall`.

    Call only after the new files are in place, so `keep` protects them.
    """
    identity = _identity(manifest)
    keep = frozenset(f["path"] for f in manifest["files"])
    if identity is not None:
        for installed in list_installed_manifests(sysroot):
            if _identity(installed) != identity or installed["qualified_name"] == manifest["qualified_name"]:
                continue
            remove_manifest_files(installed, sysroot, keep=keep)
            stale = installed_manifest_path(sysroot, installed["qualified_name"])
            if os.path.isfile(stale):
                os.remove(stale)

    write_manifest(manifest, installed_manifest_path(sysroot, manifest["qualified_name"]))


def list_installed_manifests(sysroot: str) -> list[dict]:
    """Read all installed manifests from the sysroot.

    Unreadable entries are skipped rather than raising: a single truncated
    manifest from an interrupted install would otherwise break every
    subsequent install, since every install consults this list.
    """
    mdir = installed_manifest_dir(sysroot)
    if not os.path.isdir(mdir):
        return []
    manifests = []
    for fname in sorted(os.listdir(mdir)):
        if not fname.endswith(".json"):
            continue
        try:
            manifests.append(read_manifest(os.path.join(mdir, fname)))
        except (json.JSONDecodeError, OSError) as exc:
            # Skip so one truncated manifest doesn't brick every plume command, but loudly:
            # this package's files are now invisible to conflict checks and uninstall.
            print(f"plume: warning: skipping unreadable manifest {fname}: {exc}", file=sys.stderr)
            continue
    return manifests


def check_conflicts(manifest: dict, sysroot: str, exclude_pkg: str | None = None) -> list[tuple[str, str]]:
    """Check if files in manifest conflict with other installed packages.

    Returns a list of (conflicting_file_path, owning_package_qualified_name).
    """
    new_files = {f["path"] for f in manifest["files"]}
    conflicts = []
    for installed in list_installed_manifests(sysroot):
        # Skip the package's own prior install, including one recorded under a
        # different qualifier (version bump, or newly board-varying): replacing
        # your own files is an upgrade, not a conflict. save_installed_manifest
        # supersedes that record immediately afterwards.
        own_identity = _identity(manifest)
        if installed["qualified_name"] == exclude_pkg or (
            own_identity is not None and _identity(installed) == own_identity
        ):
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
