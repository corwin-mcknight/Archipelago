"""Boot image assembly: ISO (QEMU/EDK2) and SD (U-Boot EFI on real boards)."""

import os
import subprocess
import sys

from plume.config import Config

# Removable-media EFI executable name per architecture, as Limine ships them.
EFI_APP = {"x86_64": "BOOTX64.EFI", "riscv64": "BOOTRISCV64.EFI"}


def assemble_image(config: Config, verbose: bool = False):
    """Build the target's boot image from the sysroot.

    The `image:` stanza's `format` selects the layout: `iso` (default) for the
    El Torito ISO QEMU boots, `sd` for the MBR+FAT image U-Boot's EFI path
    boots on real boards.
    """
    fmt = config.get("image", {}).get("format", "iso")
    if fmt == "iso":
        return assemble_iso(config, verbose)
    if fmt == "sd":
        return assemble_sd(config, verbose)
    print(f"error: unknown image format '{fmt}'", file=sys.stderr)
    return False


def _run_steps(steps, env, verbose):
    """Run a list of argv commands, surfacing captured output on failure."""
    capture = {} if verbose else {"stdout": subprocess.PIPE, "stderr": subprocess.STDOUT, "text": True}
    for argv in steps:
        result = subprocess.run(argv, env=env, **capture)
        if result.returncode != 0:
            print(f"error: {argv[0]} failed: {' '.join(argv)}", file=sys.stderr)
            if not verbose and result.stdout:
                print(result.stdout, end="")
            return False
    return True


def assemble_iso(config: Config, verbose: bool = False):
    """Build a bootable ISO from the sysroot.

    The target config's `image:` stanza names the boot images (sysroot-relative
    paths installed by the boot packages): `efi_boot` is required; `bios_boot`
    is present only on targets with BIOS boot, and its presence also triggers
    the limine bios-install step using the host tool from boot/limine-tools.
    """
    sysroot = config.get("sysroot")
    tools_path = config.get("tools_path")
    image_output = config.get("image_output")
    image = config.get("image", {})
    bios_boot = image.get("bios_boot")
    efi_boot = image.get("efi_boot")

    if not efi_boot:
        print("error: config has no image.efi_boot entry", file=sys.stderr)
        return False

    capture = {} if verbose else {"stdout": subprocess.PIPE, "stderr": subprocess.STDOUT, "text": True}

    # 1. Create ISO with xorriso (boot bins are already in sysroot/boot/).
    os.makedirs(os.path.dirname(image_output), exist_ok=True)
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
        sysroot, "-o", image_output,
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
        result = subprocess.run([limine_bin, "bios-install", image_output], **capture)
        if result.returncode != 0:
            print("error: limine bios-install failed", file=sys.stderr)
            if not verbose and result.stdout:
                print(result.stdout, end="")
            return False

    return True


def assemble_sd(config: Config, verbose: bool = False, output: str = None):
    """Build an SD-card image from the sysroot: MBR with one FAT32 ESP.

    The layout is what firmware EFI loaders (U-Boot's included) scan for: the
    sysroot copied in verbatim -- limine.conf at the root, the kernel and
    modules under /boot -- plus Limine's EFI executable at /EFI/BOOT/. Built
    with mtools, so no loop devices or root privileges are needed. Written to
    the config's image_output unless `output` overrides it; dd the result to
    a card, or hand it to QEMU as a raw drive.
    """
    sysroot = config.get("sysroot")
    image_output = output or config.get("image_output")
    efi_app = EFI_APP.get(config.get_arch())
    if efi_app is None:
        print(f"error: no EFI app name known for arch '{config.get_arch()}'", file=sys.stderr)
        return False
    efi_app_path = os.path.join(config.get("tools_path"), "limine-tools", efi_app)
    if not os.path.isfile(efi_app_path):
        print(f"error: {efi_app_path} missing; run `plume build`", file=sys.stderr)
        return False

    # ponytail: fixed 64 MiB, several times the current sysroot; mcopy fails
    # loudly on overflow, so grow this constant when it does.
    size_mib = 64

    os.makedirs(os.path.dirname(image_output), exist_ok=True)
    with open(image_output, "wb") as f:
        f.truncate(size_mib * 1024 * 1024)

    # mpartition and drive-letter mcopy need a drive definition; point one at
    # the image via a private MTOOLSRC so the user's own config is untouched.
    mtoolsrc = image_output + ".mtoolsrc"
    with open(mtoolsrc, "w", encoding="utf-8") as f:
        f.write(f'drive c: file="{image_output}" partition=1\nmtools_skip_check=1\n')
    env = dict(os.environ, MTOOLSRC=mtoolsrc)

    sysroot_entries = [os.path.join(sysroot, name) for name in sorted(os.listdir(sysroot))]
    steps = [
        ["mpartition", "-I", "c:"],                    # empty MBR partition table
        ["mpartition", "-c", "-a", "-T", "0xEF", "c:"],  # one whole-disk ESP-typed partition
        ["mformat", "-F", "c:"],                       # FAT32
        ["mcopy", "-s", "-b", *sysroot_entries, "c:/"],
        ["mmd", "c:/EFI", "c:/EFI/BOOT"],
        ["mcopy", "-b", efi_app_path, "c:/EFI/BOOT/"],
    ]
    try:
        return _run_steps(steps, env, verbose)
    finally:
        os.remove(mtoolsrc)
