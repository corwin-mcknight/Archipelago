"""Build stamps: one content hash decides staleness.

After a successful build, plume records the config's build hash and a
per-file content hash of every input tree -- the package's own files under
repo/packages/ for every package, plus the live source tree for live-source
packages. A package is stale exactly when that record differs from the
present: a build-affecting config change, or a source file whose content
changed, appeared, or vanished. Mtimes are never consulted, so branch
switches that restore identical content do not rebuild, and deleting a
source file is a change like any other. Make handles file-level
incrementality via .d deps.
"""

import functools
import hashlib
import json
import os


STAMP_NAME = ".plume-stamp"


@functools.cache
def _tree_hashes(source_path: str) -> dict[str, str]:
    """Content hash of every file under *source_path*, keyed by relative path.

    Cached for the life of the invocation: staleness is checked several times
    per package per command, and several packages share the kernel tree. A
    file edited after the first walk is seen by the next invocation.
    """
    hashes = {}
    for root, _dirs, files in os.walk(source_path):
        for name in files:
            path = os.path.join(root, name)
            h = hashlib.sha256()
            try:
                with open(path, "rb") as f:
                    for chunk in iter(lambda: f.read(65536), b""):
                        h.update(chunk)
            except OSError:
                continue  # vanished between walk and read (editor temp files)
            hashes[os.path.relpath(path, source_path)] = h.hexdigest()[:16]
    return hashes


def _current_files(source_paths) -> dict[str, str]:
    """Merged input hashes across all trees, keyed by absolute path."""
    files = {}
    for source_path in source_paths:
        for rel, digest in _tree_hashes(source_path).items():
            files[os.path.join(source_path, rel)] = digest
    return files


def is_stale(stamp_path: str, build_hash: str, source_paths: list[str] = ()) -> str | None:
    """Return why the package recorded at *stamp_path* needs a rebuild, or None if fresh."""
    if not os.path.exists(stamp_path):
        return "never built"

    try:
        with open(stamp_path, "r", encoding="utf-8") as f:
            recorded = json.load(f)
    except (json.JSONDecodeError, OSError):
        return "stamp unreadable"

    if recorded.get("config") != build_hash:
        return "config changed"

    old = recorded.get("files", {})
    new = _current_files(source_paths)
    for path in old.keys() | new.keys():
        if old.get(path) == new.get(path):
            continue
        rel = min((os.path.relpath(path, sp) for sp in source_paths), key=len) if source_paths else path
        if path not in new:
            return f"source removed: {rel}"
        if path not in old:
            return f"source added: {rel}"
        return f"source changed: {rel}"

    return None


def update(stamp_path: str, build_hash: str, source_paths: list[str] = ()):
    """Record a successful build, creating parent directories if needed.

    Input hashes come from the pre-build walk (the invocation cache), so a
    file edited mid-build reads as changed on the next run -- conservative,
    never stale-fresh.
    """
    os.makedirs(os.path.dirname(stamp_path), exist_ok=True)
    with open(stamp_path, "w", encoding="utf-8") as f:
        json.dump({"config": build_hash, "files": _current_files(source_paths)}, f)
        f.write("\n")
