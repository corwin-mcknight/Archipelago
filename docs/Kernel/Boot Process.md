# Boot Process

Archipelago boots via the [Limine](https://github.com/limine-bootloader/limine) bootloader protocol.
Limine loads the kernel ELF into the higher half, sets up a Higher-Half Direct Map (HHDM), and transfers control to the kernel entry point.

## Boot Sequence
The entry point is `_start` for the kernel image. The sequence is as follows:

### 1. Early Boot
Runs on the Bootstrap Processor (BSP) before any other core is started.

1. **Early heap** -- A block allocator with splitting, reuse, and coalescing is initialized using a dedicated region in BSS
   (between the `_initial_heap_start` and `_initial_heap_end` linker symbols).
   This provides `new`/`delete` before the page allocator is available.
   See [[Memory Subsystem#Early Heap]].
2. **Global constructors** -- C++ global objects are initialized via the `.init_array` section.
3. **UART** -- The serial port (COM1, 38400 baud, 8-N-1) is initialized; the log flushes to it directly.
   All kernel log output goes here.
   See [[Device Drivers]].
4. **Core discovery** -- The BSP reads Limine's MP (multiprocessor) response to discover available CPU cores.

### 2. Physical Memory

The boot processor registers the bootloader memory map with the Physical Memory Manager and initializes the virtual memory manager. Once the kernel page tables and shared zero page are available, the slab heap takes over general allocation from the early heap. See [[Memory Subsystem#Physical Memory Manager]].

### 3. SMP Bringup

The boot processor initializes its local interrupt and timer support, starts the secondary cores through the Limine MP protocol, and waits at the core initialization gate. Each secondary sets up its local execution context and interrupt entry before reporting initialization. It then waits for the scheduler to come online.

The kernel uses each CPU's position in Limine's list as its logical index. Both hardware ID lookup and secondary CPU startup panic on a missing list or out-of-range index, including in release builds.

### 4. Scheduler Startup

The boot processor initializes the object system and scheduler. Each core adopts its boot context as its idle thread. The secondary cores start their local timers and begin taking work from the shared run queue. See [Scheduling](../Design/Scheduling.md#multicore-startup-and-idle).

### 5. Kernel Entry

The kernel starts the background page-zeroing thread. The boot command line selects whether to start userspace immediately or first enter the kernel shell:

- With neither shell token, boot launches the userspace coordinator (`init`).
- `shell` starts the kernel shell as a thread and holds userspace startup until `boot continue`.
- `shell+boot` starts the shell alongside userspace startup.

The shell modes require `CONFIG_KERNEL_SHELL`; if it is disabled, boot proceeds directly to userspace.
`CONFIG_KERNEL_TESTING` enables the shell's test commands without changing the boot mode. The host harness drives those commands through the shell protocol. See [[Testing]] and [[Tasks]].

## Limine Requests
The kernel communicates with Limine through request structures placed in the `__limine_requests` linker section (`limine.cpp`).

| Request | Purpose |
|---------|---------|
| HHDM | Higher-Half Direct Map base address |
| MP | Multiprocessor core information |
| Memory map | Physical memory ranges and ownership |
| Executable file | Kernel ELF image used for symbol discovery |
| Command line | Boot-mode selection |
| Modules | Initial userspace images and their roles |
| Device tree | Firmware hardware description; currently consumed by the JH7110 PLIC driver |
| Date at boot | Initial wall-clock epoch |
| Framebuffer | First firmware framebuffer and pixel layout |
| Paging mode (riscv64) | Requires the Sv39 mode implemented by the RISC-V page-table code |

## First userspace task
The kernel's boot-only loader starts `init.bin` once. Its fixed-layout `ARCHINIT` header describes text, constants, writable data, and zero-filled storage; it is not ELF. Limine supplies exactly two userspace modules, tagged `init` and `initrd`. Init receives one read-only VMO over the opaque initrd, its exact byte length, and the initial construction capability through its bootstrap channel. Failed archive delivery terminates init and reports a boot failure.

Init validates the uncompressed ustar archive in userspace, finds `bootstrap/elf_loader.elf`, and transfers construction authority to that service after starting it. Other `bootstrap/<service>.elf` entries start through the loader; files elsewhere in the archive do not start automatically. All subsequent ELF loading runs in loader worker threads. The kernel accepts prepared mappings and initial execution parameters, never an executable image. See [[Executable Loading]].
