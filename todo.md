# TODO

## Boot & Platform
- Harden the Limine boot flow with sanity checks, fallback paths, and logging for loader failures.
- Add basic hardware discovery for ACPI tables, SMP startup, and bootstrap CPU diagnostics.
- Introduce optional kernel address space layout randomization (kASLR) and verify relocation tooling.

## Kernel Core
- Finalize the panic/log pipelines with severity filtering, buffered sinks, and crash dump emission.
- Provide structured panic artefacts suitable for post-mortem debugging and automated triage.
- Add kernel configuration flags to gate experimental features and optimize build permutations.

## Memory Management
- Implement physical memory discovery, NUMA awareness, and reserved region handling.
- Complete paging and virtual memory manager interfaces, including page table helpers.
- Deliver slab allocators and the unified heap backed by the Archipelago Unified Memory Interface.
- Add guard pages, allocation poisoning, and deterministic scrubbing for debugging hardening.

## Scheduler & Concurrency
- Build the per-CPU scheduler with run queues, priority balancing, and idle thread handling.
- Implement context switching, timeslice accounting, and cross-CPU load balancing.
- Provide kernel synchronization primitives (spinlocks, mutexes) with contention diagnostics.

## Handles & Syscalls
- Complete the handle table with entitlement enforcement, auditing, and lifecycle management.
- Define the syscall ABI, establish the userspace entry stub, and implement dispatch plumbing.
- Provide handle duplication, transfer, and revocation flows with capability checks.

## Process & Thread Lifecycle
- Introduce process and thread creation, teardown, and signal or exception propagation.
- Implement the ELF loader, user-mode transition path, and privilege boundary verification.
- Supply debug metadata for stack unwinding and cooperative crash reporting.

## IPC & Services
- Design the core message passing or channel API with capability-aware routing.
- Add shared memory/VMO duplication rules, lifetime management, and coherence guarantees.
- Define service discovery, registration, and policy enforcement for core daemons.

## Storage & Filesystem
- Implement the package store mount path and signed read-only root filesystem driver.
- Plan writable user partition support with journaling, snapshots, or rollback safeguards.
- Add a block device abstraction layer with caching and asynchronous I/O plumbing.

## Device Drivers
- Expand x86_64 bring-up with APIC/IOAPIC, HPET, and interrupt controller configuration.
- Harden UART routines, add keyboard and framebuffer/console drivers, and capture early logs.
- Implement storage (AHCI or NVMe), RTC, entropy, and watchdog timer drivers.

## Security & Reliability
- Enforce memory zeroisation, W^X policies, and static analysis for privileged code paths.
- Integrate boot-time integrity checks for packages and kernel binaries.
- Add watchdogs, assertion escalation policies, and structured fault isolation reporting.

## Testing & QA
- Grow unit, integration, and stress suites covering kernel core, drivers, and IPC subsystems.
- Automate QEMU smoke tests, add CI integration, and track coverage metrics.
- Provide fuzzing or chaos harness hooks for scheduler, memory, and syscall interfaces.
- Expand targeted coverage for: `core/cxx.cpp`, `core/interrupts.cpp`, `core/log.cpp`, `core/panic.cpp`, `core/time.cpp`.
- Extend test coverage for `std/ctype.cpp`, `std/stdlib.cpp`, `std/string.cpp` boundary conditions.
- Add scenario coverage for `x86_64/descriptor_tables.cpp`, `x86_64/drivers/pit.cpp`, `x86_64/interrupts.cpp`, `x86_64/main.cpp`, `x86_64/test_runner.cpp`, and `x86_64/uart.cpp`.

## Tooling & Developer Experience
- Provide scripts for log capture, tracing, and structured debugging workflows.
- Document usage of GDB/LLDB with QEMU, and keep `make clangd` artefacts current.
- Improve build caching, dependency tracking, and multi-target build matrix support.

## Documentation & Governance
- Expand architecture and design docs to cover scheduler, memory management, and handle models.
- Publish coding standards, contribution guidelines, and security model documentation.
- Maintain a public roadmap, change log, and stakeholder communication cadence.

## Release & Distribution
- Define semantic versioning, artefact signing, and release validation workflows.
- Automate ISO publishing, mirroring, and provenance tracking.
- Craft a regression gate checklist with performance, security, and compatibility sign-off.
