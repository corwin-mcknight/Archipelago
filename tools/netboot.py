#!/usr/bin/env python3
"""Generate the jh7110 network-boot artifacts into the TFTP root (build/riscv64/jh7110).

The board's SPI U-Boot fetches boot.scr over TFTP each boot (see the one-time
env below), which re-stages the freshest kernel, Limine, and config onto the
SD card's FAT partition and boots Limine from it. Any failure falls through to
the Debian install on the SD card.

One-time U-Boot env (serial console, then `saveenv`):
  setenv archi_netboot 'setenv autoload no; dhcp; if tftpboot 0x44000000 ${archi_server}:boot.scr; then source 0x44000000; fi'
  setenv archi_server <tftp-server-ip>
  setenv bootcmd 'run archi_netboot; run sdk_boot_env; run distro_boot_env; run distro_boot_env_test;'

Usage: netboot.py [--server 10.0.0.26]
"""

import argparse
import pathlib
import struct
import subprocess
import time
import zlib

TFTP_ROOT = pathlib.Path(__file__).resolve().parent.parent / "build/riscv64/jh7110"
LOAD = "0x44000000"
FAT = "mmc 1:1"  # the SD card's /boot partition, shared with Debian

LIMINE_CONF = """\
timeout: 0
quiet: yes
serial: yes

/Archipelago
    protocol: limine
    kaslr: no
    path: boot():/kernel.elf
    cmdline: shell
    module_path: boot():/init.elf
    module_string: init
    module_path: boot():/selftest.elf
    module_string: selftest
    module_path: boot():/echo.elf
    module_string: echo
"""

# tftp name on the server -> file name staged on the SD FAT partition
FILES = [
    ("limine-netboot.conf", "limine.conf"),
    ("boot/kernel.elf", "kernel.elf"),
    ("boot/init.elf", "init.elf"),
    ("boot/selftest.elf", "selftest.elf"),
    ("boot/echo.elf", "echo.elf"),
    ("EFI/BOOT/BOOTRISCV64.EFI", "limine.efi"),
]


def uimage_script(text: bytes) -> bytes:
    """Wrap script text as a legacy uImage (mkimage -A riscv -T script) so `source` accepts it."""
    payload = struct.pack(">II", len(text), 0) + text
    header = struct.pack(
        ">7I4B32s",
        0x27051956,  # magic
        0,  # header crc, patched below
        int(time.time()),
        len(payload),
        0,  # load address (unused for scripts)
        0,  # entry point (unused for scripts)
        zlib.crc32(payload),
        5,  # os: linux (mkimage's default for scripts)
        26,  # arch: riscv
        6,  # type: script
        0,  # compression: none
        b"archipelago netboot",
    )
    header = header[:4] + struct.pack(">I", zlib.crc32(header)) + header[8:]
    return header + payload


def boot_script(server: str) -> str:
    # One && chain: `source` runs every line regardless of the previous line's
    # status, so a single command is the only way a failed fetch skips the boot.
    steps = []
    for tftp_name, fat_name in FILES:
        steps.append(f"tftpboot {LOAD} {server}:{tftp_name}")
        steps.append(f"fatwrite {FAT} {LOAD} {fat_name} ${{filesize}}")
    steps.append(f"fatload {FAT} {LOAD} limine.efi")
    steps.append(f"bootefi {LOAD} ${{fdtcontroladdr}}")
    return " && ".join(steps) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", default="10.0.0.26", help="TFTP server IP baked into boot.scr")
    args = parser.parse_args()

    sd = TFTP_ROOT / "sd.img"
    mbr = sd.read_bytes()[:512]
    lba = struct.unpack("<I", mbr[454:458])[0]  # first partition's start sector
    subprocess.run(
        ["mcopy", "-s", "-o", "-i", f"{sd}@@{lba * 512}", "::/EFI", "::/boot", str(TFTP_ROOT)],
        check=True,
    )
    (TFTP_ROOT / "limine-netboot.conf").write_text(LIMINE_CONF)
    (TFTP_ROOT / "boot.scr").write_bytes(uimage_script(boot_script(args.server).encode()))
    print(f"netboot artifacts written to {TFTP_ROOT} (server {args.server})")


if __name__ == "__main__":
    main()
