"""Effective native tool selection and content-based build identity."""

import functools
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import shutil


TOOL_DEFAULTS = {"CC": "clang", "CXX": "clang++", "LD": "ld.lld", "AS": "nasm",
                 "AR": "llvm-ar", "OBJCOPY": "llvm-objcopy", "MAKE": "make"}
FLAG_KEYS = ("CPPFLAGS", "CFLAGS", "CXXFLAGS", "LDFLAGS", "ASFLAGS", "CWARNINGS", "COVERAGE")


def selected_tools(config):
    return {key: config.get(key.lower(), default) for key, default in TOOL_DEFAULTS.items()}


@functools.cache
def _executable_digest(path, metadata):
    """Cache a digest only while the resolved file's identity and metadata match.

    Realpath distinguishes Homebrew upgrades; inode/ctime also catch replacement
    at an unchanged path even when the caller preserves size and modification time.
    The content digest identifies custom compiler builds with unchanged versions.
    """
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def executable_identity(command):
    words = shlex.split(command)
    path = shutil.which(words[0]) if words else None
    if not path:
        return {"command": command, "missing": True}
    path = os.path.realpath(path)
    stat = os.stat(path)
    metadata = (stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns, stat.st_ctime_ns)
    return {"command": command, "path": path, "content": _executable_digest(path, metadata)}


def build_identity(config, package):
    """Hash effective commands, host ABI, flags, and Make recipe content.

    Native tools have one installed stamp shared by target configurations. Their
    identity deliberately omits the target triple/board while retaining the host
    compiler and build mode; target artifacts retain the entire config hash.
    """
    tools = selected_tools(config)
    identity = {"schema": 1, "host": [platform.system(), platform.machine()],
                "tools": {name: executable_identity(command) for name, command in tools.items()},
                "flags": {key: config.get(key.lower(), os.environ.get(key, "")) for key in FLAG_KEYS}}
    if not package.is_build_tool:
        identity["config"] = config.build_hash
    roots = [Path(config.get("repo_path")) / "packages" / package.category / package.name]
    if package.supports_live_sources and package.live_source_path:
        roots.append(Path(config.get("source_dir")) / package.live_source_path)
    recipes = []
    for root in roots:
        for path in sorted(root.rglob("*")):
            if path.is_file() and (path.name == "Makefile" or path.suffix == ".mk"):
                recipes.append((str(path.relative_to(root)), hashlib.sha256(path.read_bytes()).hexdigest()))
    if not package.is_build_tool and "lib/crt" in package.dependencies:
        for name in ("arch.mk", "user.mk"):
            path = Path(config.get("sysroot")) / "usr/lib" / name
            if path.is_file():
                recipes.append((name, hashlib.sha256(path.read_bytes()).hexdigest()))
    identity["recipes"] = recipes
    return hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()[:16]
