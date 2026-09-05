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

### 2. Per-Core Setup (`core_init`)
Each core (BSP first, then APs) runs `core_init`:

* CPU specific setup for each core is done
* Interrupts are enabled
* Some device drivers are initialized.

### 3. SMP Bringup
Application Processors (APs) are started via the Limine MP protocol.
Each AP runs `core_init` with its core ID and then enters a halt loop.
The BSP waits for all APs to signal initialization before proceeding.

### 4. Physical Memory
Memory regions from the bootloader's memory map are registered with the Physical Memory Manager.
The page allocator is now available.
The virtual memory manager initializes on top of it -- page descriptors, the kernel's own page tables, and the shared zero page -- and the slab heap then takes over general allocation from the early heap.
See [[Memory Subsystem#Physical Memory Manager]].

### 5. Kernel Entry
The kernel initializes the object system and scheduler, makes the boot context the idle thread, and starts the background page-zeroing thread.
The boot command line selects whether to start userspace immediately or first enter the kernel shell:

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
