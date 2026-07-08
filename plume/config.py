"""Load and resolve build configuration."""

import hashlib
import json
import os
import yaml


class Config:
    """Load a target config from repo/config/<arch>.yaml and resolve all paths.

    The selected config is usually reached through the ./default.yaml symlink
    in the project root (see `plume set-config`), so the path is resolved to
    its real location -- repo/config/<name>.yaml -- before deriving the
    project root two directories above it.
    """

    def __init__(self, config_path):
        with open(config_path, "r", encoding="utf-8") as f:
            data = yaml.safe_load(f)

        self.config = data["config"]
        real_path = os.path.realpath(config_path)
        self.project_root = os.path.abspath(os.path.join(os.path.dirname(real_path), "..", ".."))

        # Resolve relative paths against project root
        path_keys = [
            "sysroot", "build_dir", "tmp_path", "tools_path",
            "source_dir", "repo_path", "iso_output", "firmware",
        ]
        for key in path_keys:
            if key in self.config:
                self.config[key] = os.path.normpath(
                    os.path.join(self.project_root, self.config[key])
                )

        # Hash the build-affecting settings so stamps can detect config
        # changes. Paths are machine-specific and excluded; qemu/memory/image
        # only affect how the result is run or assembled, not how it compiles.
        hashed = {
            k: v for k, v in self.config.items()
            if k not in path_keys and k not in ("qemu", "memory", "image")
        }
        digest = hashlib.sha256(json.dumps(hashed, sort_keys=True).encode("utf-8"))
        self.build_hash = digest.hexdigest()[:16]

    def get(self, key, default=None):
        return self.config.get(key, default)

    def get_arch(self):
        return self.config.get("arch", "x86_64")
