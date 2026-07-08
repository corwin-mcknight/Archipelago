"""Build stamps: config-hash + source-mtime staleness for built packages.

After a successful build, plume writes the config's build hash to a stamp
file in the package workdir.  A package is stale when the stamp is missing,
when the recorded hash no longer matches the active config (toolchain or
flag change), or -- for live-source packages -- when any source file is
newer than the stamp.  Make handles file-level incrementality via .d deps.
"""

import os


STAMP_NAME = ".plume-stamp"


def is_stale(stamp_path: str, build_hash: str, source_path: str | None = None) -> bool:
    """Return True if the package recorded at *stamp_path* needs a rebuild."""
    if not os.path.exists(stamp_path):
        return True

    with open(stamp_path, "r", encoding="utf-8") as f:
        if f.read().strip() != build_hash:
            return True

    if source_path is None:
        return False

    stamp_mtime = os.path.getmtime(stamp_path)
    for root, _dirs, files in os.walk(source_path):
        for name in files:
            if os.path.getmtime(os.path.join(root, name)) > stamp_mtime:
                return True

    return False


def update(stamp_path: str, build_hash: str):
    """Record a successful build, creating parent directories if needed."""
    os.makedirs(os.path.dirname(stamp_path), exist_ok=True)
    with open(stamp_path, "w", encoding="utf-8") as f:
        f.write(build_hash + "\n")
