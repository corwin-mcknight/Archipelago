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
    description: str = ""
    is_build_tool: bool = False
    supports_live_sources: bool = False
    live_source_path: Optional[str] = None
    dependencies: list = field(default_factory=list)
    source: dict = field(default_factory=dict)

    @staticmethod
    def parse(full_name: str, data: dict) -> "Package":
        """Parse a package from its manifest key and YAML data."""
        info = Package.split_name(full_name)
        return Package(
            full_name=full_name,
            category=info["category"],
            name=info["name"],
            version=info["version"],
            description=data.get("description", ""),
            is_build_tool=data.get("is_build_tool", False),
            supports_live_sources=data.get("supports_live_sources", False),
            live_source_path=data.get("live_source_path"),
            dependencies=data.get("dependencies", []),
            source=data.get("source", {}),
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
        return self.full_name
