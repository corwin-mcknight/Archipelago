# TODO

## Second Architecture (riscv64)
- CLINT/PLIC interrupt routing (the trap handler dispatches raw scause codes with no external-interrupt claim path).
- SBI timer hardcodes QEMU virt's 10 MHz timebase; read the DTB's /cpus/timebase-frequency for real hardware (VisionFive 2 is 4 MHz).
- Secondary harts via Limine MP (riscv64/cpu.cpp is single-hart today).
- Grow the riscv64/tests/ suite (external interrupt claim path, more sfence/TLB behavior).
- Pick a CI system; local-first candidates to investigate: Jenkins, Woodpecker, Gitea Actions, Buildbot. `plume test --arch all` is the entry point either way.

## Boot & Platform
- `_start` still owns the boot ordering itself: `kernel::platform::console_init()`/`timer_init()` moved device bring-up out of the arch files, but the sequence (heap, ctors, console, cores, memory, traps, timer, late boot) is written twice, once per arch, and the two orders differ in ways that are not all forced. Factor the common spine once the third arch makes the real variation visible.
- Board device discovery is still hardcoded: `riscv64/platforms/virt/` fixes the timebase at QEMU virt's 10 MHz and the UART at 0x10000000. Real hardware needs the DTB (VisionFive 2 is 4 MHz); Limine has a DTB request, so extend `boot_info` rather than teaching shared code a second protocol.
- No non-default board target exists yet; adding one is a short overlay config (`base:` plus the paths that differ) plus an `<arch>/platforms/<board>/` directory. It should land together so the target means something rather than aliasing the default board's kernel.
- Linker scripts stayed in `<arch>/`: the higher-half load address is an arch and boot-protocol fact, not a board one. Revisit only if a board needs a different load address.
- Add ACPI table discovery (RSDP/MADT parsing) and bootstrap CPU diagnostics; SMP startup via Limine's MP protocol is already implemented.
- Introduce optional kernel address space layout randomization (kASLR) and verify relocation tooling.

## Kernel Core
- Add severity filtering to the log pipeline (compile-time and/or runtime min-level threshold); buffered sinks and crash dump emission are already done.
- Log renderer reaches into fixed_string internals (m_buffer) to format the timestamp/color prefix, and the 32-byte prefix buffer is sized by eyeball -- format through the type's interface and static_assert the worst case.

## Code Hygiene
- Unify naming: types mix CamelCase (HandleTable), snake_case (page_frame_allocator), and I-prefix (IInterruptHandler); constants mix kMaxSymbols, PAGE_SIZE, and IM_MAX_HANDLERS. Convention per docs is CamelCase types / UPPER_SNAKE constants -- sweep the outliers.

## KTL & Error Handling
- Monadic-style audit remainder: register_interrupt, symbols::init, and static_vector::push_back still return void instead of a result.
- Container accessor maybe<T&> overloads (M040) -- last KTL addition proposed by the audit; vector at/front/back currently return maybe<T> by copy.
- maybe<T> stores an inline default-constructed T, so an empty maybe holds a live value and non-default-constructible types won't compile -- rework to raw storage with explicit construct/destroy (vector already works this way).
- vector::emplace_back only forwards a T&&; make it variadic in-place construction or rename it.
- Result/maybe monadic combinators (map/and_then/or_else) are const-only and operate on copies -- add rvalue-qualified overloads that move.
- rb_tree has no reverse iteration or predecessor query; find_le covers the interval-lookup need for now.

## Memory Management
- VMM is the sole consumer of PMM pages -- all user-facing allocation goes through VMM, which handles reclamation and retry on PMM exhaustion.
- Implement NUMA awareness and reserved region handling.
- Bootloader-reclaimable regions are excluded from the PMM entirely (~20MB leaked); copy live Limine data out and reclaim them explicitly once execution moves off the boot stack.
- Boot modules are never reclaimed. They are classified `KERNEL` (wired) because Limine reports the kernel image and every module under one memmap type, so a module range is not distinguishable from the memmap alone. `boot_info::modules` carries each module's address and size, which is what a future initrd path needs to hand the page-aligned interior to `pmm::add_region()` once it has consumed it; a `memory_kind::MODULE` should land with that reclaim path rather than before it.
- Large-page (2M/1G) support -- the kernel assumes 4K pages everywhere (`includes/kernel/mm/page.h`).
- Cross-CPU TLB shootdown, GLOBAL-page flush for inactive spaces, and paging-structure-cache invalidation when widening intermediate USER bits (all single-CPU scoped today).
- VMM follow-ups:
    - First-fit virtual address search for VMO bindings (map takes an explicit vaddr today).
    - Binding splitting for partial unmap/protect (whole-slot ranges only).
    - Region handle exposure + detached-state machine (task/IPC milestone).
    - Shared-frame CoW beyond the zero page (share counts on real frames arrive with VMO clone).
    - Page-table frames sit in descriptor state ACTIVE, not WIRED; revisit when eviction lands.
    - PAT programming for true write-combining (degrades to uncached today).
    - Clock replacement deferred to user-pager milestone (only pager-backed pages evictable); anonymous swap ruled out permanently. OOM = allocation failure via Result.
- Deliver slab allocators and the unified heap backed by the Archipelago Unified Memory Interface.
- Add guard pages, allocation poisoning, and deterministic scrubbing for debugging hardening.

## Scheduler & Concurrency
- Extend the round-robin scheduler to multiple cores (currently BSP-only: one run queue and one idle thread, driven from the boot core), per `docs/Design/Scheduling.md` (no priority system by design); needs LAPIC timer ticks on the APs (the LAPIC timer driver landed, but only the BSP's fires), wake IPIs, and a reaper switch-completed handshake.
- Per-CPU trace rings and accounting once AP scheduling lands (today's ring and stats assume a single scheduling core).
- Latency percentiles and richer `sched` shell views if thread counts grow beyond what the flat per-thread tables can show at a glance.
- Cross-CPU load balancing, once multi-core scheduling lands.
- Back per-core identity with a GS-based per-CPU pointer before AP scheduling replaces the current x86 CPUID/dense-index lookup; make per-core lapic_id atomic to close the bring-up read/write race.
- VMM-mapped, guard-paged kernel stacks to replace the current stack-floor tripwire.
- IST-backed exception/NMI stacks on x86 -- today a fault or NMI during the stack-overflow panic path re-enters the interrupt handler on the live emergency stack, bounded only by the crash dump's recursion guard.

## Handles & Syscalls
- Handle dispatch follow-ups (the pipeline itself landed with Milestone 1: verify-then-execute over a declarative op table, `close`/`duplicate`/`obj_info` as first operations):
    - Rights bits and type ids are kernel constants, not installed ABI; `obj_info` returns them raw, so a user program can compare but not name them. Move them into `abi/` when a program first needs to request a specific right.
    - Every operation so far is type-generic; the op table's expected-type column gets its first real user with the first task- or thread-specific operation.
    - The self-handle ABI (first-generation slots 0 and 1) leans on fresh-table allocation order; a bootstrap-message scheme should replace it if the IPC milestone reshapes startup anyway.
- Implement handle transfer between tables for cross-process capability passing.
- Add kernel-owned handle tables for internal object references.
- Add handle revocation flows for server crash cleanup.
- Per-thread IPC buffer follow-ups (buffered syscalls read only this buffer, so no user pointer crosses the boundary and no copy-in helper is needed):
    - Copy-out (kernel to user) does not exist; nothing returns data yet. It is the same page walk in the other direction.
    - The per-task size cap is a compile-time constant standing in for real per-task quotas, which belong with the task/IPC milestone. Many threads each under the cap can still pin a lot of wired memory.
    - Buffers occupy fixed slots in a reserved address-space region because the VMM has no first-fit search; 64 slots per task, one bitmap word. Revisit with first-fit.
    - Buffers are wired for the thread's life and never reclaimed under pressure, which is what makes the cached frames safe. Eviction would have to unpick that.
- Enable SMAP/SMEP on x86_64 and leave `sstatus.SUM` clear on riscv64, so a stray kernel dereference of a user address traps instead of succeeding. The kernel never intentionally reads user mappings -- the ELF loader and the IPC buffer both go through the physmap -- so nothing needs an access window today, which makes this cheap to turn on and a real backstop if something later reaches for a user pointer by mistake.
- Replace the x86_64 syscall entry's single-core stack globals with per-CPU GS state when SMP scheduling lands.

## Task & Thread Lifecycle
- Terminate only the faulting user task for unresolved user-mode faults; the current path panics the kernel until task-kill machinery exists.
- Implement task-kill and exception propagation (task/thread vocabulary per `docs/Design/Task Model.md` -- no processes, no UNIX signals).
- Extract the userspace runtime once a second user program exists. `src/sys/init/` currently owns `_start` (per arch), the syscall wrappers, the linker script, and its freestanding compile flags; each exists once, so factoring now would build a library with one caller. The second program is the trigger, and the thing to extract is a C runtime -- `_start`, syscall stubs, linker script -- as a package installing headers and a static archive next to `sys/kernel-headers`, not a libc. Nothing needs malloc, stdio, string, or locale, and naming it `libc` invites someone to supply them. Initrd will likely reshape userspace anyway, so committing late is cheaper than committing now.
- ELF loader follow-ups (static ET_EXEC for the running architecture is what loads today):
    - No `ET_DYN`/PIE support, which is the prerequisite for user-space ASLR; relocation processing is a milestone of its own.
    - Segments must be page-aligned and may not share a page. Ordinary lld output satisfies this, but a packed binary from another toolchain is rejected rather than mapped.
    - `MAX_SEGMENTS` is a fixed 8, chosen for static binaries; a real toolchain image with more loadable segments would be refused.
    - The user stack address and size are still fixed constants chosen by the kernel, not derived from the image; first-fit virtual address search is a VMM to-do.
    - `PT_GNU_STACK` is ignored -- the stack is mapped `READ|WRITE` unconditionally.
- Supply debug metadata for user-mode stack unwinding and cooperative crash reporting (kernel-side crash reporting already exists).

## IPC & Services
- Implement the message passing/channel API with capability-aware routing per the existing design in `docs/Design/IPC Primitives.md` (design is written; implementation is open).
- Add shared memory/VMO duplication rules, lifetime management, and coherence guarantees.
- Define service discovery, registration, and policy enforcement for core daemons.
- Design input and output. There are no files and there will be no file-shaped "standard input"/"standard output", so a program needs some other way to reach a device it may write to or read from. The `write` syscall is a debug convenience with no console object behind it and is not the answer; it stays a non-handle debug facility. The open part is how a program holding only its own task and thread handles obtains a channel to a device server, and what that server's interface looks like -- see `docs/Design/Syscall Interface.md`.

## Storage & Filesystem
- Implement the package store mount path and signed read-only root filesystem driver.
- Plan writable user partition support with journaling, snapshots, or rollback safeguards.
- Add a block device abstraction layer with caching and asynchronous I/O plumbing.

## Device Drivers
- Expand x86_64 bring-up with IOAPIC routing for device interrupts (LAPIC and its timer landed; the legacy PIC is now fully masked).
- High-resolution timer events: one-shot deadline programming (LAPIC one-shot/TSC-deadline on x86_64, sbi_set_timer already one-shot on riscv64) with a deadline queue, so sleeps and preemption wake at sub-tick deadlines; `ns_since_boot()` already reads the cycle counter.
- Add keyboard (PS/2) and framebuffer/console drivers; wire the Limine framebuffer request (UART hardening already landed).
- UART: pre-init panics lose their output (writes before init are dropped by the health gate); real hardware needs a bounded data-ready poll before reading the loopback echo; consider an atomic health flag for crash-context writes.
- UART RX interrupt path (IOAPIC/PLIC routing) so shell input can block on a wait queue instead of sleep-polling; QEMU's chardev backpressure makes the current 1 ms poll lossless, but a real 16550's 16-byte FIFO would drop pasted input.
- Implement storage (AHCI or NVMe), RTC, entropy, and watchdog timer drivers.

## Security & Reliability
- Enforce memory zeroisation, W^X policies, and static analysis for privileged code paths.
- Integrate boot-time integrity checks for packages and kernel binaries.
- Add watchdog firing (the crash trigger enum slot is already reserved) and structured fault isolation reporting; assertion escalation policy already exists.

## Testing & QA
- Continue growing driver/core unit and stress coverage where gaps remain; add IPC suites once the IPC subsystem exists.
- VMM test gaps: WRITE_COMBINING end-to-end (indistinguishable from DEVICE until PAT lands) and OOM paths in the fault/commit paths (needs PMM fault injection).
- Wire up a CI pipeline (no config exists yet) that runs the existing host+QEMU tiers and applies the coverage gate -- coverage tracking and QEMU test automation are already done.
- Extend the fuzz harness to memory-subsystem interfaces now; add scheduler fuzz targets (run-queue rotation, wait-queue bookkeeping, signal-mask matching) now that the scheduler exists, and syscall fuzz targets once syscalls exist.
- Harness protocol lines can interleave with concurrent log flush output (one test_end line was garbled in the 2026-06-10 run, test still counted); make @@HARNESS emission atomic with respect to log flushes.
- Expand targeted coverage for: `core/cxx.cpp`, `core/interrupts.cpp`, `core/log.cpp`, `core/panic.cpp`, `core/time.cpp`.
- KTL edge-case gaps: self-move assignment (vector/ref/Result), ref refcount-overflow panic path, negative-compilation checks for deleted overloads (e.g. maybe<T&> rvalue binding).
- Add scenario coverage for `x86_64/descriptor_tables.cpp` (GDT/IDT setup), `x86_64/apic.cpp` (LAPIC timer), and `x86_64/main.cpp` (core_init); uart and interrupt dispatch/exception paths are already covered.

## Tooling & Developer Experience
- Plume decides a package is installed from world membership plus an installed manifest newer than the build stamp, so files removed from the sysroot behind its back are never noticed: deleting `boot/kernel.elf` or `usr/include/abi/` leaves `plume build`/`install` reporting nothing to do, and the ISO is assembled without them. Verifying the manifest's files still exist would close it, at the cost of stat-ing every installed file on each invocation.
- Provide standalone scripts for ad-hoc log capture and tracing outside the test harness (the harness already captures structured logs during runs).
- Expand the Debugging doc with a concrete GDB/QEMU remote-attach walkthrough (stub port, symbol loading, break-on-entry); `make clangd` already exists.
- Kernel shell enhancements:
    - Object Inspection -- expand handle inspect and obj inspect with detailed views
    - Table Dumps -- add full handle table dump with object details
    - Runtime Metrics -- add interrupt counts, allocation stats, tick rates
    - Debugging Aids -- add memory dump, stack trace, register dump commands
- Crash handler follow-ups: shell-drop on crash, watchdog injection (enum slot reserved), #DF/triple-fault handling (needs IST), stack overflow detection (needs guard pages), SMP crash fan-out (needs IPI).

## Documentation & Governance
- Publish contribution guidelines and a security model doc (coding standards already covered by `docs/Development.md`).
- Maintain a public roadmap, change log, and stakeholder communication cadence.

## Release & Distribution
- Define semantic versioning, artefact signing, and release validation workflows.
- Automate ISO publishing, mirroring, and provenance tracking.
- Craft a regression gate checklist with performance, security, and compatibility sign-off.
