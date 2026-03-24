"""World file management — tracks packages installed into the sysroot."""

import os


class World:
    """Read and write the world file at {sysroot}/var/plume/world.

    The world file is a newline-separated list of name-version entries,
    one per installed package (e.g. "sys/kernel-0.0.1").
    """

    def __init__(self, sysroot: str):
        self.path = os.path.join(sysroot, "var", "plume", "world")

    def read(self) -> list[str]:
        """Return the list of installed package entries."""
        if not os.path.exists(self.path):
            return []
        with open(self.path, "r", encoding="utf-8") as f:
            return [line.strip() for line in f if line.strip()]

    def contains(self, full_name: str) -> bool:
        """Check whether a package is recorded in the world file."""
        return full_name in self.read()

    def add(self, full_name: str):
        """Add or update a package entry in the world file.

        If another version of the same category/name exists it is replaced.
        """
        entries = self.read()

        # Strip version to get "category/name" prefix
        prefix = _name_prefix(full_name)

        # Remove any existing entry for this package (possibly older version)
        entries = [e for e in entries if _name_prefix(e) != prefix]
        entries.append(full_name)
        entries.sort()

        self._write(entries)

    def remove(self, full_name: str):
        """Remove a package entry from the world file."""
        entries = self.read()
        prefix = _name_prefix(full_name)
        entries = [e for e in entries if _name_prefix(e) != prefix]
        self._write(entries)

    def _write(self, entries: list[str]):
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        with open(self.path, "w", encoding="utf-8") as f:
            for entry in entries:
                f.write(entry + "\n")


def _name_prefix(full_name: str) -> str:
    """Extract 'category/name' from 'category/name-version'."""
    dash = full_name.rfind("-")
    if dash == -1:
        return full_name
    return full_name[:dash]
