"""ISO image assembly — replaces tools/fs-install-limine.sh."""

import os
import shutil
import subprocess
import sys

from plume.config import Config


def assemble_iso(config: Config, verbose: bool = False):
    """Build a bootable ISO from the sysroot."""
    sysroot = config.get("sysroot")
    tools_path = config.get("tools_path")
    iso_output = config.get("iso_output")
    limine_dir = os.path.join(tools_path, "limine")

    capture = {} if verbose else {"stdout": subprocess.PIPE, "stderr": subprocess.STDOUT, "text": True}

    # 1. Copy limine boot binaries into sysroot/boot/
    #    (limine.conf and boot/ structure are installed by the limine-boot-files-x86 package)
    boot_dir = os.path.join(sysroot, "boot")
    os.makedirs(boot_dir, exist_ok=True)
    for f in ["limine-bios-cd.bin", "limine-uefi-cd.bin", "limine-bios.sys"]:
        src = os.path.join(limine_dir, f)
        if os.path.exists(src):
            shutil.copy2(src, boot_dir)

    # 2. Create ISO with xorriso
    os.makedirs(os.path.dirname(iso_output), exist_ok=True)
    result = subprocess.run([
        "xorriso", "-as", "mkisofs",
        "-b", "boot/limine-bios-cd.bin",
        "-no-emul-boot", "-boot-load-size", "4", "-boot-info-table",
        "--efi-boot", "boot/limine-uefi-cd.bin",
        "-efi-boot-part", "--efi-boot-image", "--protective-msdos-label",
        "--quiet",
        sysroot, "-o", iso_output,
    ], **capture)
    if result.returncode != 0:
        print("error: xorriso failed", file=sys.stderr)
        if not verbose and result.stdout:
            print(result.stdout, end="")
        return False

    # 3. Install limine BIOS bootcode
    limine_bin = os.path.join(limine_dir, "limine")
    result = subprocess.run([limine_bin, "bios-install", iso_output], **capture)
    if result.returncode != 0:
        print("error: limine bios-install failed", file=sys.stderr)
        if not verbose and result.stdout:
            print(result.stdout, end="")
        return False

    return True
