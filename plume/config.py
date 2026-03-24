"""Load and resolve build configuration."""

import os
import yaml


class Config:
    """Load configuration from repo/config.yaml and resolve all paths."""

    def __init__(self, config_path):
        with open(config_path, "r", encoding="utf-8") as f:
            data = yaml.safe_load(f)

        self.config = data["config"]
        self.project_root = os.path.abspath(os.path.dirname(config_path) + "/..")

        # Resolve relative paths against project root
        path_keys = [
            "sysroot", "build_dir", "tmp_path", "tools_path",
            "source_dir", "repo_path", "iso_output",
        ]
        for key in path_keys:
            if key in self.config:
                self.config[key] = os.path.normpath(
                    os.path.join(self.project_root, self.config[key])
                )

    def get(self, key, default=None):
        return self.config.get(key, default)

    def get_arch(self):
        return self.config.get("arch", "x86_64")

    def get_arch_config(self):
        arch = self.get_arch()
        return self.config.get("arch_configs", {}).get(arch, {})
