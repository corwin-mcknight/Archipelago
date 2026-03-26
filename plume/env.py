"""Build environment variable generation for package Makefiles."""

import os

from plume.config import Config
from plume.package import Package


def get_build_env(config: Config, package: Package) -> dict:
    """Construct the environment dict passed to package Make invocations."""
    env = dict(os.environ)

    arch = config.get_arch()
    tmp_base = os.path.join(config.get("tmp_path"), package.category, f"{package.name}-{package.version}")

    env["ARCH"] = arch
    env["SYSROOT"] = config.get("sysroot")
    env["BUILD_DIR"] = config.get("build_dir")
    env["FILESDIR"] = os.path.join(config.get("repo_path"), "packages", package.category, package.name)
    env["WORKDIR"] = tmp_base
    env["S"] = os.path.join(tmp_base, "src")
    env["D"] = os.path.join(tmp_base, "install")
    env["P"] = f"{package.name}-{package.version}"
    env["PN"] = package.name
    env["PV"] = package.version
    env["CATEGORY"] = package.category
    env["CC"] = config.get("cc", "clang")
    env["CXX"] = config.get("cxx", "clang++")
    env["LD"] = config.get("ld", "ld.lld")
    env["AS"] = config.get("as", "nasm")
    env["MAKE"] = config.get("make", "make")
    env["MAKE_JOBS"] = str(os.cpu_count() or 1)
    env["TOOLS_PATH"] = config.get("tools_path")

    if package.is_build_tool:
        env["TOOL_INSTALL"] = config.get("tools_path")

    if package.supports_live_sources and package.live_source_path:
        env["LIVE_SOURCES"] = os.path.join(config.get("source_dir"), package.live_source_path)

    return env
