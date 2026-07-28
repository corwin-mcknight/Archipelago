"""Package metadata model."""

import re
from dataclasses import dataclass, field
from typing import Optional


@dataclass
class Package:
    """Represents a single package in the repository."""

    full_name: str          # e.g. "sys/kernel-0.0.1"
    category: str           # e.g. "sys"
    name: str               # e.g. "kernel"
    version: str            # e.g. "0.0.1"
    arch: str = ""          # e.g. "x86_64", set at load time from config
    board: str = ""         # e.g. "virt", set at load time from config
    description: str = ""
    is_build_tool: bool = False
    supports_live_sources: bool = False
    live_source_path: Optional[str] = None
    dependencies: list = field(default_factory=list)
    source: dict = field(default_factory=dict)
    arches: list = field(default_factory=list)  # empty = all architectures
    varies_by: list = field(default_factory=list)  # axes this package builds separately for

    @property
    def supported(self) -> bool:
        """Whether this package exists for the active architecture.

        Matched against the bare arch: a board narrows a target, it never
        changes which architecture that target is.
        """
        return not self.arches or not self.arch or self.arch in self.arches

    @property
    def varies_by_board(self) -> bool:
        """Whether this package builds separately per board on the active target."""
        return bool(self.board) and "board" in self.varies_by

    @property
    def variant_suffix(self) -> str:
        """Qualifier tail beyond the arch: '^virt' for board-varying packages, else ''.

        Packages that declare no board-specific behavior resolve at the arch
        level and are shared by every board on that arch -- that is the normal
        case, not a cache miss.
        """
        return f"^{self.board}" if self.varies_by_board else ""

    @property
    def qualified_name(self) -> str:
        """Full name with target qualifier, e.g. 'sys/kernel-0.0.1~x86_64^pc'."""
        if self.arch:
            return f"{self.full_name}~{self.arch}{self.variant_suffix}"
        return self.full_name

    @staticmethod
    def parse(full_name: str, data: dict, arch: str = "", board: str = "") -> "Package":
        """Parse a package from its manifest key and YAML data."""
        info = Package.split_name(full_name)
        return Package(
            full_name=full_name,
            category=info["category"],
            name=info["name"],
            version=info["version"],
            arch=arch,
            board=board,
            description=data.get("description", ""),
            is_build_tool=data.get("is_build_tool", False),
            supports_live_sources=data.get("supports_live_sources", False),
            live_source_path=data.get("live_source_path"),
            dependencies=data.get("dependencies", []),
            source=data.get("source", {}),
            arches=data.get("arches", []),
            varies_by=data.get("varies_by", []),
        )

    @staticmethod
    def split_name(full_name: str) -> dict:
        """Split 'category/name-version' into components.

        Examples:
            'sys/kernel-0.0.1' -> {'category': 'sys', 'name': 'kernel', 'version': '0.0.1'}
            'boot/limine-10.0' -> {'category': 'boot', 'name': 'limine', 'version': '10.0'}
        """
        match = re.match(r"^(.+)/(.+)-([^-]+)$", full_name)
        if not match:
            raise ValueError(f"Invalid package name: {full_name}")
        return {
            "category": match.group(1),
            "name": match.group(2),
            "version": match.group(3),
        }

    def __hash__(self):
        return hash(self.full_name)

    def __eq__(self, other):
        return isinstance(other, Package) and self.full_name == other.full_name

    def __str__(self):
        return self.qualified_name
