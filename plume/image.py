"""ISO image assembly — replaces tools/fs-install-limine.sh."""

import os
import shutil
import subprocess
import sys

from plume.config import Config


def assemble_iso(config: Config):
    """Build a bootable ISO from the sysroot."""
    sysroot = config.get("sysroot")
    tools_path = config.get("tools_path")
    media_dir = config.get("media_dir")
    iso_output = config.get("iso_output")
    limine_dir = os.path.join(tools_path, "limine")

    # 1. Copy media files (limine.conf, etc.) into sysroot
    os.makedirs(sysroot, exist_ok=True)
    if os.path.isdir(media_dir):
        shutil.copytree(media_dir, sysroot, dirs_exist_ok=True)

    # 2. Copy limine boot binaries into sysroot/boot/
    boot_dir = os.path.join(sysroot, "boot")
    os.makedirs(boot_dir, exist_ok=True)
    for f in ["limine-bios-cd.bin", "limine-uefi-cd.bin", "limine-bios.sys"]:
        src = os.path.join(limine_dir, f)
        if os.path.exists(src):
            shutil.copy2(src, boot_dir)

    # 3. Create ISO with xorriso
    os.makedirs(os.path.dirname(iso_output), exist_ok=True)
    result = subprocess.run([
        "xorriso", "-as", "mkisofs",
        "-b", "boot/limine-bios-cd.bin",
        "-no-emul-boot", "-boot-load-size", "4", "-boot-info-table",
        "--efi-boot", "boot/limine-uefi-cd.bin",
        "-efi-boot-part", "--efi-boot-image", "--protective-msdos-label",
        "--quiet",
        sysroot, "-o", iso_output,
    ])
    if result.returncode != 0:
        print("error: xorriso failed", file=sys.stderr)
        return False

    # 4. Install limine BIOS bootcode
    limine_bin = os.path.join(limine_dir, "limine")
    result = subprocess.run([limine_bin, "bios-install", iso_output])
    if result.returncode != 0:
        print("error: limine bios-install failed", file=sys.stderr)
        return False

    return True
