# TODO

## Boot & Platform
- Add ACPI table discovery (RSDP/MADT parsing) and bootstrap CPU diagnostics; SMP startup via Limine's MP protocol is already implemented.
- Introduce optional kernel address space layout randomization (kASLR) and verify relocation tooling.

## Kernel Core
- Add severity filtering to the log pipeline (compile-time and/or runtime min-level threshold); buffered sinks and crash dump emission are already done.
- Log renderer reaches into fixed_string internals (m_buffer) to format the timestamp/color prefix, and the 32-byte prefix buffer is sized by eyeball -- format through the type's interface and static_assert the worst case.

## Code Hygiene
- Unify naming: types mix CamelCase (HandleTable), snake_case (page_frame_allocator), and I-prefix (IInterruptHandler); constants mix kMaxSymbols, PAGE_SIZE, and IM_MAX_HANDLERS. Convention per docs is CamelCase types / UPPER_SNAKE constants -- sweep the outliers.
- x86_64/main.cpp hardcodes memmap `type == 6` two lines below a symbolic LIMINE_MEMMAP_USABLE check -- use the named constant.
- Register struct fields userrsp/eflags are legacy 32-bit names for what are rsp/rflags on x86_64.
- interrupts.cpp handler entry union: clear_handler writes the function arm regardless of which arm is active (works only because both are pointers) -- clear by discriminant or memset.

## KTL & Error Handling
- Monadic-style audit follow-ups: result<void>/KTRY/errc landed 2026-06-12 and closed the findings they gated; recount the remainder (still open: register_interrupt, symbols::init, and static_vector::push_back return void).
- Container accessor maybe<T&> overloads (M040) -- last KTL addition proposed by the audit; vector at/front/back currently return maybe<T> by copy.
- maybe<T> stores an inline default-constructed T, so an empty maybe holds a live value and non-default-constructible types won't compile -- rework to raw storage with explicit construct/destroy (vector already works this way).
- vector::emplace_back only forwards a T&&; make it variadic in-place construction or rename it.
- Result/maybe monadic combinators (map/and_then/or_else) are const-only and operate on copies -- add rvalue-qualified overloads that move.

## Memory Management
- Background page-zeroing worker thread (gated on scheduler).
- VMM is the sole consumer of PMM pages -- all user-facing allocation goes through VMM, which handles reclamation and retry on PMM exhaustion.
- Wired page tracking belongs to the VMM; boot code passes kernel physical ranges to VMM init separately from PMM.
- Global page descriptor array -- deferred until VMM/VMO design is settled.
- Implement NUMA awareness and reserved region handling.
- PMM usable pool includes Limine bootloader-reclaimable regions that contain the live boot stack -- draining the PMM to exhaustion zeroes the active stack page (found 2026-06-10 while testing rollback). Defer reclaiming those regions until execution moves off them.
- Large-page (2M/1G) support -- the kernel assumes 4K pages everywhere (`includes/kernel/mm/page.h`).
- Complete paging and virtual memory manager interfaces, including page table helpers.
    - CR3 activation and kernel-mapping cloning for new address spaces.
    - TLB maintenance before CR3 activation goes live -- cross-CPU shootdown, GLOBAL-page flush for inactive spaces, and paging-structure-cache invalidation when widening intermediate USER bits.
    - EFER.NXE enablement so NO_EXECUTE mappings can stop being rejected.
    - Region tree, VMO, and pager objects.
- Deliver slab allocators and the unified heap backed by the Archipelago Unified Memory Interface.
- Add guard pages, allocation poisoning, and deterministic scrubbing for debugging hardening.

## Scheduler & Concurrency
- Build the per-CPU scheduler with run queues and idle thread handling, per the round-robin design in `docs/Design/Scheduling.md` (no priority system by design).
- Implement context switching, timeslice accounting, and cross-CPU load balancing.
- Add a kernel mutex (blocking lock) and contention diagnostics; spinlocks and a semaphore already exist in `kernel/synchronization/`.
- Back per-core identity with a GS-based per-CPU pointer when interrupt or scheduler code starts needing the calling core -- the CPUID-based current_core_index() is currently dead code with no callers; make per-core lapic_id atomic to close the bring-up read/write race.

## Handles & Syscalls
- Implement handle transfer between tables for cross-process capability passing.
- Add kernel-owned handle tables for internal object references.
- Add handle revocation flows for server crash cleanup.
- Define the syscall ABI, establish the userspace entry stub, and implement dispatch plumbing.

## Task & Thread Lifecycle
- Introduce task and thread creation, teardown, and exception/fault propagation (task/thread vocabulary per `docs/Design/Task Model.md` -- no processes, no UNIX signals).
- Implement the ELF loader, user-mode transition path, and privilege boundary verification.
- Supply debug metadata for user-mode stack unwinding and cooperative crash reporting (kernel-side crash reporting already exists).

## IPC & Services
- Implement the message passing/channel API with capability-aware routing per the existing design in `docs/Design/IPC Primitives.md` (design is written; implementation is open).
- Add shared memory/VMO duplication rules, lifetime management, and coherence guarantees.
- Define service discovery, registration, and policy enforcement for core daemons.

## Storage & Filesystem
- Implement the package store mount path and signed read-only root filesystem driver.
- Plan writable user partition support with journaling, snapshots, or rollback safeguards.
- Add a block device abstraction layer with caching and asynchronous I/O plumbing.

## Device Drivers
- Expand x86_64 bring-up with APIC/IOAPIC, HPET, and interrupt controller configuration.
- Add keyboard (PS/2) and framebuffer/console drivers; wire the Limine framebuffer request (UART hardening already landed).
- UART follow-ups from the 2026-06-10 fixes: writes before init are now dropped by the health gate (pre-init panics lose output); real hardware needs a bounded data-ready poll before reading the loopback echo; consider an atomic health flag for crash-context writes.
- Implement storage (AHCI or NVMe), RTC, entropy, and watchdog timer drivers.

## Security & Reliability
- Enforce memory zeroisation, W^X policies, and static analysis for privileged code paths.
- Integrate boot-time integrity checks for packages and kernel binaries.
- Add watchdog firing (the crash trigger enum slot is already reserved) and structured fault isolation reporting; assertion escalation policy already exists.

## Testing & QA
- Two-tier host+QEMU test system complete, steps 1-6; fuzz (5 targets) and TSan (2 targets) lanes are periodic-CI, not inner loop. No new fuzz targets queued.
- Continue growing driver/core unit and stress coverage where gaps remain; add IPC suites once the IPC subsystem exists.
- Wire up a CI pipeline (no config exists yet) that runs the existing host+QEMU tiers and applies the coverage gate -- coverage tracking and QEMU test automation are already done.
- Extend the fuzz harness to memory-subsystem interfaces now; add scheduler/syscall fuzz targets once those subsystems exist.
- Harness protocol lines can interleave with concurrent log flush output (one test_end line was garbled in the 2026-06-10 run, test still counted); make @@HARNESS emission atomic with respect to log flushes.
- Expand targeted coverage for: `core/cxx.cpp`, `core/interrupts.cpp`, `core/log.cpp`, `core/panic.cpp`, `core/time.cpp`.
- KTL edge-case gaps: self-move assignment (vector/ref/Result), ref refcount-overflow panic path, negative-compilation checks for deleted overloads (e.g. maybe<T&> rvalue binding).
- Add scenario coverage for `x86_64/descriptor_tables.cpp` (GDT/IDT setup), `x86_64/drivers/pit.cpp`, and `x86_64/main.cpp` (core_init); uart and interrupt dispatch/exception paths are already covered.

## Tooling & Developer Experience
- Provide standalone scripts for ad-hoc log capture and tracing outside the test harness (the harness already captures structured logs during runs).
- Expand the Debugging doc with a concrete GDB/QEMU remote-attach walkthrough (stub port, symbol loading, break-on-entry); `make clangd` already exists.
- Add multi-target build matrix support (build caching and dependency tracking already exist in Plume).
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
