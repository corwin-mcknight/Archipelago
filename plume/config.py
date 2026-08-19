"""Load and resolve build configuration."""

import hashlib
import json
import os
import yaml


# Settings that name paths rather than affect compilation. Excluded from the
# build hash because they are machine-specific.
PATH_KEYS = [
    "sysroot", "build_dir", "tmp_path", "tools_path",
    "source_dir", "repo_path", "image_output", "firmware",
]

# Settings excluded from the build hash for reasons other than being paths.
# qemu/memory/image only affect how a result is run or assembled. `board` is
# excluded so that board targets sharing an arch also share package staleness:
# a package that does not vary by board must not rebuild when the board
# changes. Board-varying packages get their isolation from the qualifier in
# their workdir and cache paths instead (see Package.variant_suffix).
NON_BUILD_KEYS = ("qemu", "memory", "image", "board")


def is_bare_config_name(name):
    """True if name is a bare config name -- no path separators, not '.' or '..' --
    so it can never escape the config directory when joined into a path."""
    return not (os.sep in name or (os.altsep and os.altsep in name) or name in (os.curdir, os.pardir))


class Config:
    """Load a target config from repo/config/<target>.yaml and resolve all paths.

    A target is an architecture, optionally narrowed to a board:
    `riscv64.yaml` is an arch target, `riscv64^visionfive2.yaml` is a board
    target. A board config names its arch with `base:` and declares only what
    differs; the base is merged underneath it, so toolchain settings live in
    one place and boards cannot silently drift apart.

    The selected config is usually reached through the ./default.yaml symlink
    in the project root (see `plume set-config`), so the path is resolved to
    its real location -- repo/config/<name>.yaml -- before deriving the
    project root two directories above it.
    """

    def __init__(self, config_path):
        real_path = os.path.realpath(config_path)
        self.config_path = real_path
        self.project_root = os.path.abspath(os.path.join(os.path.dirname(real_path), "..", ".."))

        self.config = _load_merged(real_path)
        # Resolved values of whatever this config inherits, so validation can
        # tell an overridden path from one silently shared with the base.
        self.base_values = _resolve_paths(_base_config(real_path), self.project_root)

        _resolve_paths(self.config, self.project_root)

        # Hash the build-affecting settings so stamps can detect config changes.
        hashed = {
            k: v for k, v in self.config.items()
            if k not in PATH_KEYS and k not in NON_BUILD_KEYS
        }
        digest = hashlib.sha256(json.dumps(hashed, sort_keys=True).encode("utf-8"))
        self.build_hash = digest.hexdigest()[:16]

    def get(self, key, default=None):
        return self.config.get(key, default)

    def get_arch(self):
        """Bare architecture, never board-qualified (e.g. 'riscv64')."""
        return self.config.get("arch", "x86_64")

    def get_board(self):
        """Board name for this target, or '' if the target names no board."""
        return self.config.get("board", "")

    def target_name(self):
        """Display name for this target: 'riscv64' or 'riscv64^visionfive2'."""
        board = self.get_board()
        return f"{self.get_arch()}^{board}" if board else self.get_arch()


def _resolve_paths(cfg, project_root):
    """Normalize every path-valued setting against the project root, in place."""
    for key in PATH_KEYS:
        if key in cfg:
            cfg[key] = os.path.normpath(os.path.join(project_root, cfg[key]))
    return cfg


def _base_config(real_path):
    """Resolved config this one inherits, or {} if it declares no base."""
    base_name = _read_config_block(real_path).get("base")
    if not base_name:
        return {}
    base_path = os.path.join(os.path.dirname(real_path), f"{base_name}.yaml")
    return _load_merged(os.path.realpath(base_path)) if os.path.isfile(base_path) else {}


def _read_config_block(path):
    with open(path, "r", encoding="utf-8") as f:
        data = yaml.safe_load(f)
    if not data or "config" not in data:
        raise ValueError(f"{path}: missing top-level 'config:' block")
    return dict(data["config"])


def _load_merged(real_path, _seen=None):
    """Read a config, recursively merging any `base:` config underneath it.

    Overlay keys win over base keys. `base:` itself is consumed by the merge
    and never reaches the resolved config, so it cannot affect the build hash.
    """
    _seen = _seen or []
    if real_path in _seen:
        chain = " -> ".join(os.path.basename(p) for p in _seen + [real_path])
        raise ValueError(f"circular config base chain: {chain}")

    cfg = _read_config_block(real_path)
    base_name = cfg.pop("base", None)
    if not base_name:
        return cfg

    # A base names a sibling config, never a path: without this the value flows
    # straight into os.path.join, where '../..' escapes the config directory and
    # an absolute path replaces it outright.
    if not is_bare_config_name(base_name):
        raise ValueError(f"{os.path.basename(real_path)}: base '{base_name}' must name a config, not a path")

    base_path = os.path.join(os.path.dirname(real_path), f"{base_name}.yaml")
    if not os.path.isfile(base_path):
        raise ValueError(f"{os.path.basename(real_path)}: base config '{base_name}' not found at {base_path}")

    base_cfg = _load_merged(os.path.realpath(base_path), _seen + [real_path])
    base_cfg.update(cfg)
    return base_cfg
