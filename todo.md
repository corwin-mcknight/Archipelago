# TODO

## Boot & Platform
- Harden the Limine boot flow with sanity checks, fallback paths, and logging for loader failures.
- Add basic hardware discovery for ACPI tables, SMP startup, and bootstrap CPU diagnostics.
- Introduce optional kernel address space layout randomization (kASLR) and verify relocation tooling.

## Kernel Core
- Finalize the panic/log pipelines with severity filtering, buffered sinks, and crash dump emission.
- ~~Provide structured panic artefacts suitable for post-mortem debugging and automated triage.~~ Done (crash subsystem with frame-pointer backtrace, register dump, log drain, harness-side symbolication).

## KTL & Error Handling
- Monadic-style audit (2026-06-10): findings on replacing sentinels/bool+out-param with maybe/Result, plus proposed KTL additions.
- ~~maybe improvements (maybe<T&>, inspect, expect/take/reset/operator bool, map_or fix, from_ptr, ok_or) and first deployments (registry lookups, BSP search, Limine chain, find_test).~~ Done 2026-06-10; 41 audit findings remain open.
- ~~Monadic search algorithms: ktl::find_if/find -> maybe<T&>, ktl::find_index_if -> maybe<size_t>; deployed to find_bsp_index, find_test, find_command, TypeRegistry lookups.~~ Done 2026-06-10.
- ~~result<void,E>, KTRY, errc unification~~ Done 2026-06-12. Remaining KTL addition from the audit: container accessor maybe<T&> overloads (M040).
- Live bugs found by that audit: `mm/pmm.h` add_region ignores push_back failure (corrupts page accounting); `ktl/fixed_string` string_view ctor leaves no null terminator when the view fills the buffer; `ktl/static_vector` at() bounds-checks against capacity instead of size; `kernel/assert.h:23` warn-arm format typo `(1})`.
- `ktl/fmt` parser: a format spec with width but no specifier (e.g. `{0:8}`) silently swallows all output until the next `}` -- the post-spec scan skips past the closing brace it is already on. Either support the specifier-less form or make it fail loudly. Found 2026-06-10.
- ~~`ktl::span`, lazy ranges (`enumerate`, `filter`, `transform`, `take`, `drop`) landed; `cpu.cpp` migrated to span + views.~~ Done 2026-06-15.

## Memory Management
- ~~PMM refactor -- strip allocator down to a simple free-pool manager.~~ Done.
- Background page-zeroing worker thread (gated on scheduler).
- VMM is the sole consumer of PMM pages -- all user-facing allocation goes through VMM, which handles reclamation and retry on PMM exhaustion.
- Wired page tracking belongs to the VMM; boot code passes kernel physical ranges to VMM init separately from PMM.
- Global page descriptor array -- deferred until VMM/VMO design is settled.
- Implement NUMA awareness and reserved region handling.
- PMM usable pool includes Limine bootloader-reclaimable regions that contain the live boot stack -- draining the PMM to exhaustion zeroes the active stack page (found 2026-06-10 while testing rollback). Defer reclaiming those regions until execution moves off them.
- Complete paging and virtual memory manager interfaces, including page table helpers.
    - ~~x86_64 page-table primitives (init/destroy, map_page, walk, unmap_page).~~ Done.
    - CR3 activation and kernel-mapping cloning for new address spaces.
    - TLB maintenance before CR3 activation goes live -- cross-CPU shootdown, GLOBAL-page flush for inactive spaces, and paging-structure-cache invalidation when widening intermediate USER bits.
    - EFER.NXE enablement so NO_EXECUTE mappings can stop being rejected.
    - Region tree, VMO, and pager objects.
- Deliver slab allocators and the unified heap backed by the Archipelago Unified Memory Interface.
- Add guard pages, allocation poisoning, and deterministic scrubbing for debugging hardening.

## Scheduler & Concurrency
- Build the per-CPU scheduler with run queues, priority balancing, and idle thread handling.
- Implement context switching, timeslice accounting, and cross-CPU load balancing.
- Provide kernel synchronization primitives (spinlocks, mutexes) with contention diagnostics.
- Replace the CPUID-based current-core lookup in interrupt dispatch with a GS-based per-CPU pointer (CPUID serializes and VM-exits on every interrupt); make per-core lapic_id reads atomic to silence the formal bring-up race.

## Handles & Syscalls
- ~~Implement handle table with create, duplicate, close, type-safe get, and generation counters.~~ Done.
- ~~Implement type registry with registration, lookup, and live object accounting.~~ Done.
- ~~Implement Event and Counter kernel object types.~~ Done.
- ~~Cap handle rights at creation time against the type's valid rights mask.~~ Done (create_handle rejects unregistered types and any rights outside the type's valid_rights).
- Implement handle transfer between tables for cross-process capability passing.
- Add kernel-owned handle tables for internal object references.
- Add handle revocation flows for server crash cleanup.
- Define the syscall ABI, establish the userspace entry stub, and implement dispatch plumbing.

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
- UART follow-ups from the 2026-06-10 fixes: writes before init are now dropped by the health gate (pre-init panics lose output); real hardware needs a bounded data-ready poll before reading the loopback echo; consider an atomic health flag for crash-context writes.
- Implement storage (AHCI or NVMe), RTC, entropy, and watchdog timer drivers.

## Security & Reliability
- Enforce memory zeroisation, W^X policies, and static analysis for privileged code paths.
- Integrate boot-time integrity checks for packages and kernel binaries.
- Add watchdogs, assertion escalation policies, and structured fault isolation reporting.

## Testing & QA
- Two-tier (host + QEMU) test system. Steps 1-3 landed 2026-06-19: host-tier fork-per-test runner under ASan/UBSan (`make host-test`), expression-capturing EXPECT/REQUIRE with the legacy KTEST_* macros aliased over it, minimal `registry.h` cross-tier ABI, shared `@@HARNESS` aggregator. Full migration complete -- routing is directory-based (no migrated-test list to keep in sync): `tests/*.cpp` = host-tier (built by the runner), `tests/freestanding/` = kernel env (built only by the kernel), `tests/runner/` = host harness. Split: 187 on host + 72 in QEMU = 259, each test on one tier. `std_string` + `symbols` reclassified to freestanding (their C-linkage string funcs resolve to libc on host = vacuous; symbols needs the embedded symbol table + crash path). obj_* migrated via compiling the object-system sources in + no-op interrupt stubs for the spinlock. Remaining: (4) QEMU brute-isolation reboot-per-test + shard onto the shared aggregator, (5) diagnostics MVP+ (coverage gate, peak-RSS, JUnit), (6) staged fuzz/TSan lanes (extracting a pure demangler out of crash.cpp would let `symbols` be host-fuzzed).
- Grow unit, integration, and stress suites covering kernel core, drivers, and IPC subsystems.
- Automate QEMU smoke tests, add CI integration, and track coverage metrics.
- Provide fuzzing or chaos harness hooks for scheduler, memory, and syscall interfaces.
- Harness protocol lines can interleave with concurrent log flush output (one test_end line was garbled in the 2026-06-10 run, test still counted); make @@HARNESS emission atomic with respect to log flushes.
- Expand targeted coverage for: `core/cxx.cpp`, `core/interrupts.cpp`, `core/log.cpp`, `core/panic.cpp`, `core/time.cpp`.
- Extend test coverage for `std/ctype.cpp`, `std/stdlib.cpp`, `std/string.cpp` boundary conditions.
- Add scenario coverage for `x86_64/descriptor_tables.cpp`, `x86_64/drivers/pit.cpp`, `x86_64/interrupts.cpp`, `x86_64/main.cpp`, and `x86_64/uart.cpp`.

## Tooling & Developer Experience
- Provide scripts for log capture, tracing, and structured debugging workflows.
- Document usage of GDB/LLDB with QEMU, and keep `make clangd` artefacts current.
- Improve build caching, dependency tracking, and multi-target build matrix support.
- ~~Kernel Shell, built in shell that merges the testing suite with an interactive command line for diagnostics and experimentation of the kernel.~~ Done.
    - ~~Built in commands to poke VMM, scheduler, handle tables, and other core subsystems.~~ Done (mem, handle, obj, cpu, log commands).
    - Object Inspection -- expand handle inspect and obj inspect with detailed views
    - Table Dumps -- add full handle table dump with object details
    - Runtime Metrics -- add interrupt counts, allocation stats, tick rates
    - Debugging Aids -- add memory dump, stack trace, register dump commands
- ~~Kernel crash handler that captures structured crash dumps, logs, and optionally drops into the kernel shell for post-mortem analysis.~~ Done except shell-drop on crash. Future work: watchdog injection (enum slot reserved), #DF/triple-fault handling (needs IST), stack overflow detection (needs guard pages), SMP crash fan-out (needs IPI).
- ~~In-kernel symbolication for crash backtrace -- snapshot kernel ELF's symbol table at boot via Limine executable_file_request.~~ Done.
## Documentation & Governance
- Expand architecture and design docs to cover scheduler, memory management, and handle models.
- Publish coding standards, contribution guidelines, and security model documentation.
- Maintain a public roadmap, change log, and stakeholder communication cadence.

## Release & Distribution
- Define semantic versioning, artefact signing, and release validation workflows.
- Automate ISO publishing, mirroring, and provenance tracking.
- Craft a regression gate checklist with performance, security, and compatibility sign-off.
