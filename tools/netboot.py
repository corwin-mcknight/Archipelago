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
import hashlib
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
    # The fetch is one && chain so a failed transfer skips the stamp write and
    # the boot falls through to the SD. The stamp (a hash of the artifact set)
    # is fetched first and compared against the SD's copy; a match skips the
    # transfers entirely. It is written last, after the artifacts it describes.
    alt = "0x45000000"
    fetch = " && ".join(
        f"tftpboot {LOAD} {server}:{tftp_name} && fatwrite {FAT} {LOAD} {fat_name} ${{filesize}}"
        for tftp_name, fat_name in FILES
    )
    # Keep the display alive across ExitBootServices. This SPI U-Boot tags its
    # DC8200 (video) and Inno HDMI (display) drivers DM_FLAG_OS_PREPARE (0x400),
    # so efi_exit_boot_services runs their .remove -- powering the panel off
    # right as Limine takes over. Clearing the flag on both relocated struct
    # driver .flags fields skips that teardown, so the framebuffer Limine hands
    # the kernel stays scanned out. Addresses are fixed for this build (relocaddr
    # 0xf7f17000); each write is guarded on the current 0x400 value so a U-Boot
    # reflash that moves the structs is left untouched rather than corrupted.
    keep_display = (
        "if itest.l *f7fd54d0 == 0x400; then mw.l f7fd54d0 0; fi\n"
        "if itest.l *f7fd4648 == 0x400; then mw.l f7fd4648 0; fi\n"
    )
    return f"""setenv autoload no
dhcp
setenv stamp_ok 0
if tftpboot {LOAD} {server}:netboot.stamp; then
setenv stamp_size ${{filesize}}
if fatload {FAT} {alt} netboot.stamp && itest ${{filesize}} == ${{stamp_size}} && cmp.b {LOAD} {alt} ${{stamp_size}}; then
setenv stamp_ok 1
fi
fi
if itest ${{stamp_ok}} == 1; then
echo netboot: artifacts current, skipping transfers
else
{fetch} && tftpboot {LOAD} {server}:netboot.stamp && fatwrite {FAT} {LOAD} netboot.stamp ${{filesize}}
fi
{keep_display}fatload {FAT} {LOAD} limine.efi && bootefi {LOAD} ${{fdtcontroladdr}}
"""


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
    digest = hashlib.sha256()
    for tftp_name, _ in FILES:
        digest.update((TFTP_ROOT / tftp_name).read_bytes())
    (TFTP_ROOT / "netboot.stamp").write_text(digest.hexdigest() + "\n")
    print(f"netboot artifacts written to {TFTP_ROOT} (server {args.server})")


if __name__ == "__main__":
    main()
