"""ISO image assembly."""

import os
import subprocess
import sys

from plume.config import Config


def assemble_iso(config: Config, verbose: bool = False):
    """Build a bootable ISO from the sysroot.

    The target config's `image:` stanza names the boot images (sysroot-relative
    paths installed by the boot packages): `efi_boot` is required; `bios_boot`
    is present only on targets with BIOS boot, and its presence also triggers
    the limine bios-install step using the host tool from boot/limine-tools.
    """
    sysroot = config.get("sysroot")
    tools_path = config.get("tools_path")
    iso_output = config.get("iso_output")
    image = config.get("image", {})
    bios_boot = image.get("bios_boot")
    efi_boot = image.get("efi_boot")

    if not efi_boot:
        print("error: config has no image.efi_boot entry", file=sys.stderr)
        return False

    capture = {} if verbose else {"stdout": subprocess.PIPE, "stderr": subprocess.STDOUT, "text": True}

    # 1. Create ISO with xorriso (boot bins are already in sysroot/boot/).
    os.makedirs(os.path.dirname(iso_output), exist_ok=True)
    xorriso_args = ["xorriso", "-as", "mkisofs"]
    if bios_boot:
        xorriso_args += [
            "-b", bios_boot,
            "-no-emul-boot", "-boot-load-size", "4", "-boot-info-table",
        ]
    xorriso_args += [
        "--efi-boot", efi_boot,
        "-efi-boot-part", "--efi-boot-image", "--protective-msdos-label",
        "--quiet",
        sysroot, "-o", iso_output,
    ]
    result = subprocess.run(xorriso_args, **capture)
    if result.returncode != 0:
        print("error: xorriso failed", file=sys.stderr)
        if not verbose and result.stdout:
            print(result.stdout, end="")
        return False

    # 2. Install limine BIOS bootcode (host tool from boot/limine-tools)
    if bios_boot:
        limine_bin = os.path.join(tools_path, "limine-tools", "limine")
        result = subprocess.run([limine_bin, "bios-install", iso_output], **capture)
        if result.returncode != 0:
            print("error: limine bios-install failed", file=sys.stderr)
            if not verbose and result.stdout:
                print(result.stdout, end="")
            return False

    return True
