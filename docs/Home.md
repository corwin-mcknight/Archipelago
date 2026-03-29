# Archipelago
Archipelago is a minimal, security-focused operating system written in freestanding C++20. It boots via the [Limine](https://github.com/limine-bootloader/limine) bootloader and runs in QEMU.

This vault mixes documentation for the current system with documentation for the planned architecture.
Today, Archipelago provides a small kernel with boot, memory-management, testing, and bootstrap driver support. The planned architecture is a microkernel built around typed kernel objects, capability handles, userspace servers, and programmable fast paths. The system is built with [[Plume]], a custom package manager.
## Current System
- [[Boot Process]]: Limine handoff, early heap, SMP bringup
- [[Memory Subsystem]]: Current allocators plus planned virtual memory design
- [[Interrupt Model]]: Current interrupt dispatch plus planned object-model direction
- [[Device Drivers]]: Planned userspace driver model plus today's bootstrap UART/PIT drivers
- [[KTL]]: Freestanding container library
- [[Configuration]]: Compile-time constants
- [[Testing]]: Test framework and harness
## Development
- [[Development]]: Devcontainer setup, toolchain, IDE, code style
- [[Plume]]: Package manager and build system
## Planned Architecture
The following pages describe the long-term design for Archipelago. These features are not yet implemented.
- [[Design Principles]]: Core philosophy
- [[Object Model]]: Typed kernel objects and capability handles
- [[Handle Table]]: Per-process handle management
- [[IPC Primitives]]: Inter-process communication
- [[Object Transaction Programs]]: Programmable dispatch optimization
- [[Server Lifecycle]]: Server registration and crash recovery
