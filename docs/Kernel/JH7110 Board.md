# JH7110 Board
The StarFive JH7110 (Orange Pi RV, VisionFive 2) is the first physical board Archipelago runs on: four SiFive U74 cores at 1.5 GHz, LPDDR4, a 16550-compatible UART0, and an SPI-flash vendor U-Boot (2021.10 + OpenSBI v1.2). This page records what is true only of that board; the generic boot chain is in [[Boot Process]] and build commands are in `BUILDING.md`.

## Development Loop
The board is never flashed during iteration. Its SPI U-Boot environment runs `archi_netboot` from `bootcmd`: DHCP on the wired port, `tftpboot` a `boot.scr` from the development host, and source it, falling through to the vendor Debian on the SD card if anything fails. `tools/netboot.py` generates that script and `limine-netboot.conf` into `build/riscv64/jh7110/`, which the host serves over TFTP (`dnsmasq --port=0 --enable-tftp`). The script fetches the kernel, modules, config, and Limine EFI binary and writes them onto the SD card's FAT partition, then `bootefi`s Limine from there. Loading Limine from a real volume matters: booted straight from RAM under the vendor U-Boot it hits a broken PXE path and stops at a "no boot volume" keypress gate.

- `make netboot` rebuilds the image and refreshes the TFTP root; `reboot` at the kernel shell then netboots the new build in about ten seconds.
- `make console` attaches an interactive serial session (Ctrl-] detaches).
- `make board-test [TEST=name]` runs the QEMU test tiers on the board over serial (`tools/board_test.py`).

Serial reaches the devcontainer through a TCP bridge, because container runtimes cannot pass the USB-UART adapter through: `socat` on the host exposes the adapter on a port, and `tools/serial_mux.py` fans that single connection out so automation and an interactive console can share it. Only one client may talk to the raw bridge at a time -- multiple clients race for received bytes.

## Reset and Watchdog
SBI system reset does not work on this board: OpenSBI's reset path talks to the AXP15060 PMIC over I2C, and U-Boot gates those clocks off at EFI handoff, so the call fails. The kernel instead reboots by arming the hardware watchdog (an SP805-style block in `riscv64/platforms/jh7110/watchdog.cpp`) with an immediate timeout. The same watchdog runs all the time with a 60 s timeout and a feeder thread, so a hang or panic resets the board and netboots again without intervention. The power button is needed only after a true power cut; the PMIC's configuration is volatile and cannot be fixed in firmware.

## Display
HDMI output comes from U-Boot's DC8200 and Inno HDMI drivers, for which no public register documentation exists, so the kernel cannot modeset itself. It relies on U-Boot leaving the display scanned out and Limine handing over the framebuffer. The vendor U-Boot marks both drivers `DM_FLAG_OS_PREPARE`, which makes `ExitBootServices` power the video output domain off; `boot.scr` clears that flag on both relocated driver structs before `bootefi`. The writes are guarded by an `itest` on the expected flag value so a different U-Boot build is left untouched -- the addresses are specific to the binary in SPI.

The framebuffer console (`core/console.cpp`) follows two rules learned on this hardware:

- Never read from or scroll within the scanned-out aperture. It is write-only from the kernel's point of view; the console keeps a character-cell grid in RAM and blits only dirty cells.
- The display controller scans DRAM directly while the U74 data cache is write-back, and the JH7110 has neither Zicbom nor Svpbmt, so cached writes never reach the panel on their own. `kernel::platform::dcache_clean_range` writes each line's physical address to the SiFive composable-cache `Flush64` register, the same mechanism mainline Linux uses for non-coherent DMA on this SoC. Any future DMA engine on the board needs the same flush; QEMU and x86_64 are coherent and implement it as a no-op.

## UART Quirks
The 16550 driver originally failed the board's loopback self-test because it read the echoed byte without waiting (instant only in emulators), and wrote the line-control register while U-Boot's transmit FIFO was still draining, which the DesignWare UART rejects. Both are fixed in `core/uart_16550.cpp`; keep them in mind when touching that driver.
