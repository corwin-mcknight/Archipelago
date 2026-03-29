# Boot Process

Archipelago boots via the [Limine](https://github.com/limine-bootloader/limine) bootloader protocol.
Limine loads the kernel ELF into the higher half, sets up a Higher-Half Direct Map (HHDM), and transfers control to the kernel entry point.

## Boot Sequence
The entry point is `_start` for the kernel image. The sequence is as follows:

### 1. Early Boot
Runs on the Bootstrap Processor (BSP) before any other core is started.

1. **Early heap** -- A bump allocator is initialized using a dedicated region in BSS
   (between the `_initial_heap_start` and `_initial_heap_end` linker symbols).
   This provides `new`/`delete` before the page allocator is available.
   See [[Memory Subsystem#Early Heap]].
2. **Global constructors** -- C++ global objects are initialized via the `.init_array` section.
3. **UART** -- The serial port (COM1, 38400 baud, 8-N-1) is initialized and registered as a logging device.
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
See [[Memory Subsystem#Physical Memory Manager]].

### 5. Kernel Entry
If `CONFIG_KERNEL_TESTING` is enabled (the default), the kernel enters the test runner.
See [[Testing]].

Otherwise, the kernel currently panics. There is no scheduler or userspace yet.

## Limine Requests
The kernel communicates with Limine through request structures placed in the `__limine_requests` linker section (`limine.cpp`).

| Request | Purpose |
|---------|---------|
| HHDM | Higher-Half Direct Map base address |
| MP | Multiprocessor core information |
