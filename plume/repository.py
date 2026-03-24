"""Load packages from packages.yml."""

import yaml
from plume.package import Package


def load_packages(packages_yml_path: str) -> list[Package]:
    """Load all packages from a packages.yml file."""
    with open(packages_yml_path, "r", encoding="utf-8") as f:
        data = yaml.safe_load(f)

    packages = []
    for full_name, pkg_data in data.items():
        packages.append(Package.parse(full_name, pkg_data or {}))
    return packages
