"""World file management — tracks packages installed into the sysroot."""

import os


class World:
    """Read and write the world file at {sysroot}/var/plume/world.

    The world file is a newline-separated list of qualified name entries,
    one per installed package (e.g. "sys/kernel-0.0.1~x86_64").
    Backward-compatible with unqualified entries from older installs.
    """

    def __init__(self, sysroot: str):
        self.path = os.path.join(sysroot, "var", "plume", "world")

    def read(self) -> list[str]:
        """Return the list of installed package entries."""
        if not os.path.exists(self.path):
            return []
        with open(self.path, "r", encoding="utf-8") as f:
            return [line.strip() for line in f if line.strip()]

    def contains(self, name: str) -> bool:
        """Check whether a package is recorded in the world file.

        Matches by category/name prefix so both qualified and unqualified
        entries are found regardless of arch suffix.
        """
        prefix = _name_prefix(name)
        return any(_name_prefix(e) == prefix for e in self.read())

    def add(self, name: str):
        """Add or update a package entry in the world file.

        If another version of the same category/name exists it is replaced.
        """
        entries = self.read()

        # Strip version (and arch) to get "category/name" prefix
        prefix = _name_prefix(name)

        # Remove any existing entry for this package (possibly older version)
        entries = [e for e in entries if _name_prefix(e) != prefix]
        entries.append(name)
        entries.sort()

        self._write(entries)

    def remove(self, name: str):
        """Remove a package entry from the world file."""
        entries = self.read()
        prefix = _name_prefix(name)
        entries = [e for e in entries if _name_prefix(e) != prefix]
        self._write(entries)

    def _write(self, entries: list[str]):
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        with open(self.path, "w", encoding="utf-8") as f:
            for entry in entries:
                f.write(entry + "\n")


def _name_prefix(entry: str) -> str:
    """Extract 'category/name' from 'category/name-version[~arch]'."""
    # Strip ~arch if present
    if "~" in entry:
        entry = entry.rsplit("~", 1)[0]
    dash = entry.rfind("-")
    if dash == -1:
        return entry
    return entry[:dash]
