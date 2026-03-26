"""Source-tree stamp for incremental builds of live-source packages.

After a successful build, plume touches a stamp file in the package workdir.
Before the next build, it walks the live source tree and checks whether any
file is newer than the stamp.  If so, the package is stale and Make is
re-invoked (which handles file-level incrementality via .d deps).
"""

import os
from pathlib import Path


STAMP_NAME = ".plume-stamp"


def is_stale(source_path: str, stamp_path: str) -> bool:
    """Return True if any file under *source_path* is newer than *stamp_path*.

    Also returns True when the stamp does not yet exist (first build).
    """
    if not os.path.exists(stamp_path):
        return True

    stamp_mtime = os.path.getmtime(stamp_path)

    for root, _dirs, files in os.walk(source_path):
        for name in files:
            if os.path.getmtime(os.path.join(root, name)) > stamp_mtime:
                return True

    return False


def update(stamp_path: str):
    """Touch the stamp file, creating parent directories if needed."""
    os.makedirs(os.path.dirname(stamp_path), exist_ok=True)
    Path(stamp_path).touch()
