"""ISO image assembly."""

import os
import subprocess
import sys

from plume.config import Config


def assemble_iso(config: Config, verbose: bool = False):
    """Build a bootable ISO from the sysroot.

    Expects boot binaries (limine-bios-cd.bin, etc.) and limine.conf to
    already be installed in the sysroot by the boot/limine and
    boot/limine-config packages.  The host-side limine tool (for
    bios-install) comes from boot/limine-tools in the tools directory.
    """
    sysroot = config.get("sysroot")
    tools_path = config.get("tools_path")
    iso_output = config.get("iso_output")

    capture = {} if verbose else {"stdout": subprocess.PIPE, "stderr": subprocess.STDOUT, "text": True}

    # 1. Create ISO with xorriso (boot bins are already in sysroot/boot/)
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

    # 2. Install limine BIOS bootcode (host tool from boot/limine-tools)
    limine_bin = os.path.join(tools_path, "limine", "limine")
    result = subprocess.run([limine_bin, "bios-install", iso_output], **capture)
    if result.returncode != 0:
        print("error: limine bios-install failed", file=sys.stderr)
        if not verbose and result.stdout:
            print(result.stdout, end="")
        return False

    return True
