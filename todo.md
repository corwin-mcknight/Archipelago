# TODO

## Next Up
- Server dispatch / capability-aware routing: the next IPC design piece now that channels, handle transfer, ports, and wait timeouts all exist (see `docs/Design/IPC Primitives.md` and `docs/Design/Object Model.md`).

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
- Post-Milestone-1 review findings, architecture and boot:
    - `fp_walk_result` is declared identically in both arch crash files and `core/crash.cpp`, and the whole `crash::arch` interface is declared in a `.cpp` rather than `kernel/crash.h`, so the implementations are never checked against it. `is_canonical`, `in_kernel_half`, and the register-format `emit()` helper are duplicated verbatim too.
    - riscv64 calls `fault_enter()` only inside the page-fault branch while x86_64 calls it for every exception, so identical faults reach the crash path with different fault depth and different `blocking_allowed()` behaviour.
    - `trap_sp_overflow` on x86_64 branches out before `isr_common`'s `cld`, so the panic and crash-dump path runs with DF in whatever state the faulting context left it.
    - The riscv64 board timebase is defined in both `platforms/virt/platform.cpp` and `platforms/virt/timer.cpp`, and `timer.cpp` re-implements `rdtime()` instead of calling `kernel::arch::timestamp()`.
    - `enable_nxe()`'s comment claims it must run before any NX mapping is installed, but `init_memory()` clones and activates Limine's page tables (NX bits included) first; it survives only because Limine already set EFER.NXE. Fix the order or the comment. `MSR_EFER` is also defined in both `main.cpp` and `arch.cpp`.
    - The `gdts[]` array has no alignment attribute while the IDT does; `struct gdt` nests only packed members, so entries land at arbitrary alignment.
    - `.init_array` is placed in the executable `text` PHDR on both arches; the constructor pointer table belongs in `:rodata`. The unexplained `. += 0x1000;` in `.bss` also deserves a comment naming what it pads.
    - Boot memmap entries are trusted for overlap: an overlapping USABLE entry would hand kernel-image frames to the PMM (wrapping ranges are now rejected in `init_memory`, and usable ranges past the descriptor cap are dropped from the PMM rather than left allocatable-but-uncovered). `cpu_hw_id`/`start_cpu` bound-check with `assert`, which compiles out under NDEBUG.
    - Dead includes in `x86_64/arch.cpp` (log, panic, time, ioport), `riscv64/arch.cpp` (panic), and `descriptor_tables.cpp` (`kernel/cpu.h`).
    - The board source glob in the kernel Makefile picks up `*.cpp` and `*.S` but not `*.s`, the extension every existing x86 assembly file uses, so a board assembly file would be silently dropped from the link.

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
    - Zero non-test callers, candidates for deletion: `ktl::tuple` (whole header), `take_view`/`drop_view`/`views::transform`, eight of the eleven `ktl::bit` functions, `maybe`'s `filter`/`take`/`ptr_or`/`from_ptr` and its duplicate non-const `has_value()`, `static_vector::peek_back()`, and the `strcpy`/`strncpy`/`strlcpy`/`atoi`/ctype surface in the std shims.

## Memory Management
- VMM is the sole consumer of PMM pages -- all user-facing allocation goes through VMM, which handles reclamation and retry on PMM exhaustion.
- Implement NUMA awareness and reserved region handling.
- Bootloader-reclaimable regions are excluded from the PMM entirely (~20MB leaked); copy live Limine data out and reclaim them explicitly once execution moves off the boot stack.
- Boot modules are never reclaimed. They are classified `KERNEL` (wired) because Limine reports the kernel image and every module under one memmap type, so a module range is not distinguishable from the memmap alone. `boot_info::modules` carries each module's address and size, which is what a future initrd path needs to hand the page-aligned interior to `pmm::add_region()` once it has consumed it; a `memory_kind::MODULE` should land with that reclaim path rather than before it.
- Large-page (2M/1G) support -- the kernel assumes 4K pages everywhere (`includes/kernel/mm/page.h`).
- Cross-CPU TLB shootdown, GLOBAL-page flush for inactive spaces, and paging-structure-cache invalidation when widening intermediate USER bits (all single-CPU scoped today).
- Memory-pressure signal userspace can wait on: a kernel Event with level bits (low, critical) sampled where the zeroer already runs, so servers drop caches before allocations start failing -- the kernel cannot reclaim server-held memory itself and anonymous memory is never swapped. Open questions: hysteresis at the boundary, global level versus per-task, and whether ignoring it has a consequence.
- VMM follow-ups:
    - First-fit virtual address search for VMO bindings (map takes an explicit vaddr today).
    - Binding splitting for partial unmap/protect (whole-slot ranges only).
    - Region handle exposure + detached-state machine (task/IPC milestone).
    - Shared-frame CoW beyond the zero page (share counts on real frames arrive with VMO clone).
    - Page-table frames sit in descriptor state ACTIVE, not WIRED; revisit when eviction lands.
    - PAT programming for true write-combining (degrades to uncached today).
    - Clock replacement deferred to user-pager milestone (only pager-backed pages evictable); anonymous swap ruled out permanently. OOM = allocation failure via Result.
- Post-Milestone-1 review findings, memory management:
    - `page_frame_allocator::free` validates nothing: it accepts unaligned addresses, double frees, and frees of WIRED/MMIO frames, even though `g_page_descriptors` already knows each frame's state.
    - `Region::protect` starts its scan at `lower_bound(base)`, so a binding straddling the range start keeps its old wider protection and the call still returns ok. `unmap` already does the partial-overlap pre-scan this needs.
    - `map_page`/`unmap_page` accept kernel-half addresses, where intermediate tables are shared across every address space, so a kernel-half map on a user aspace would mutate all of them and `arch_destroy` would leak the tables.
    - The "OOM = allocation failure via Result" contract is now real after `heap_activate()`: the nothrow `operator new` returns null on slab-heap exhaustion. The early heap still panics, but only the boot window (pre-PMM) runs on it. Remaining fallout: audit existing callers that assume allocation success now that null is actually deliverable (deferred to review).
    - `early_heap` guards its block list with interrupts off rather than a lock, which is mutual exclusion only while one core allocates. Upgrading to a spinlock needs a constant-initialised lock, because `on_boot()` runs before the global constructors.
    - The `total_consumed > block->size` rejection in `early_heap::alloc` is unreachable; the preceding `usable < size` check already guarantees it.
    - The vmo constructor ignores chunk-index allocation failure, producing a VMO whose `size_pages()` exceeds what its index covers; the grow path in `set_size` handles the same failure correctly.
    - `create_device_vmo` marks its range WIRED before the vmo exists and nothing ever un-marks it, so a failed construction or a destroyed device VMO leaves the range permanently WIRED.
    - Both arch `flush_tlb_page` implementations duplicate the same active-root guard and its comment; only the invalidate instruction differs.
    - `page_descriptor.h`'s `coverage_end()` hardcodes `0x1000` instead of `KERNEL_MINIMUM_PAGE_SIZE`.
- Heap large-path ceiling: multi-page allocations use `alloc_contiguous`, which only carves untouched region tails, and freed runs return as single pages -- heavy multi-page churn slowly consumes contiguous capacity. A PMM that tracks free runs (buddy or equivalent) lifts this.
- The host page-source stub caps live large runs at 4096 entries.
- Remaining AUMI phases over the arenas (`mm/object_arena.cpp`): allocation hardening (poisoning, redzones, a guard-page debug mode) and per-CPU magazines when SMP scheduling lands.
- Arena client deferred: handle-table entries. The table is a contiguous `ktl::vector`, so feeding it from an arena means converting it to chunked entry batches -- a standalone refactor of the verification path.

## Scheduler & Concurrency
- Extend the round-robin scheduler to multiple cores (currently BSP-only: one run queue and one idle thread, driven from the boot core), per `docs/Design/Scheduling.md` (no priority system by design); needs LAPIC timer ticks on the APs (the LAPIC timer driver landed, but only the BSP's fires), wake IPIs, and a reaper switch-completed handshake.
- Per-CPU trace rings and accounting once AP scheduling lands (today's ring and stats assume a single scheduling core).
- Latency percentiles and richer `sched` shell views if thread counts grow beyond what the flat per-thread tables can show at a glance.
- Cross-CPU load balancing, once multi-core scheduling lands.
- SMP park/wake handshake (decided: per-thread on-cpu flag, Linux style): block_if's unlock-to-switch window is safe only on a single scheduling core. When AP scheduling lands, each thread gets an on-cpu flag cleared from sched_finish_switch once its state is fully saved; a waker that finds the thread parked spins until the flag clears before running it. Chosen over holding the queue lock across the switch so the never-switch-while-holding-a-lock discipline stays intact.
- Back per-core identity with a GS-based per-CPU pointer before AP scheduling replaces the current x86 CPUID/dense-index lookup; make per-core lapic_id atomic to close the bring-up read/write race.
- VMM-mapped, guard-paged kernel stacks to replace the current stack-floor tripwire.
- Post-Milestone-1 review findings, scheduler and synchronization:
    - `lockdep` mutates the per-CPU held-lock stack non-atomically with interrupts enabled for mutex guards; an ISR taking any tracked spinlock would corrupt it. Latent until the planned UART RX interrupt path lands.
    - The self-handle window in `create_user_task` relies on an unenforced invariant: `spawn_into` enqueues the payload before the handles are inserted, and anything that blocks in between lets the thread run without ABI slots 0 and 1. Split spawn into create + enqueue so the handles land first.
    - Self-handle insert failure is only a warning and the task still starts; if the first insert fails and the second succeeds, a Thread occupies slot 0 and the documented ABI becomes type confusion. Route it through the existing `fail()` path.
    - `switch_to` publishes `g_kstack_floor` and the TSS stack while still running on the outgoing stack, so a fault in that window is checked against the incoming thread's floor. Publish from `sched_finish_switch`, which already runs first on the incoming stack.
    - `switch_to` republishes the syscall kernel stack only when `kstack_top() != 0`, leaving the previous thread's value live for stackless threads; publish unconditionally with a poison value so a stray syscall faults.
    - `spawn` counts and traces a spawn before the run-queue push whose failure unwinds it, so failed spawns are recorded as real.
    - `register_task` swallows push failure with `(void)`, unlike every other scheduler queue push, which asserts.
    - `yield()` and `service_pending_preemption()` duplicate the same pop-next / demote / requeue / switch sequence, differing only in stat counter and reason.
    - Stale assert text: `service_pending_preemption` reports "on_tick: run queue allocation failed".
    - Dead: `execution_context::irq_depth` is write-only bookkeeping never read by `blocking_allowed` or anything else, `assert_thread_context` has no callers, and `synchronization::semaphore` has no users, duplicates `obj::Semaphore`, and busy-waits in a way that would hard-hang a single core.
- IST-backed exception/NMI stacks on x86 -- today a fault or NMI during the stack-overflow panic path re-enters the interrupt handler on the live emergency stack, bounded only by the crash dump's recursion guard.

## Handles & Syscalls
- Handle dispatch follow-ups (the pipeline itself landed with Milestone 1: verify-then-execute over a declarative op table, `close`/`duplicate`/`obj_info` as first operations):
    - Rights bits and type ids are kernel constants, not installed ABI; `obj_info` returns them raw, so a user program can compare but not name them. Move them into `abi/` when a program first needs to request a specific right.
    - Every operation so far is type-generic; the op table's expected-type column gets its first real user with the first task- or thread-specific operation.
    - The self-handle ABI (first-generation slots 0 and 1) leans on fresh-table allocation order; a bootstrap-message scheme should replace it if the IPC milestone reshapes startup anyway.
    - Self-handles are unowned (`HandleTable::insert_unowned`), and the slot-1 thread entry stays safe only because reap of the initial thread tears down the whole task; a thread-spawn syscall must close the self-thread entry when its thread is reaped, or the entry dangles.
- Add kernel-owned handle tables for internal object references.
- Add handle revocation flows for server crash cleanup.
- Per-thread IPC buffer follow-ups (buffered syscalls read only this buffer, so no user pointer crosses the boundary and no copy-in helper is needed):
    - The per-task size cap is a compile-time constant standing in for real per-task quotas, which belong with the task/IPC milestone. Many threads each under the cap can still pin a lot of wired memory.
    - Buffers occupy fixed slots in a reserved address-space region because the VMM has no first-fit search; 64 slots per task, one bitmap word. Revisit with first-fit.
    - Buffers are wired for the thread's life and never reclaimed under pressure, which is what makes the cached frames safe. Eviction would have to unpick that.
- Post-Milestone-1 review findings, syscall and handle pipeline:
    - `syscall.cpp` reaches a task through `static_ref_cast<Task>(self->owner())` with no type check; `Thread` accepts any `Object` owner, so a non-Task owner is silent type confusion. Same pattern in `scheduler.cpp` and `reaper.cpp`.
    - `SYS_SLEEP` passes its argument straight into `now() + ticks`, so a large value wraps to a deadline in the past and returns on the next tick.
    - `sys_write`'s copy loop runs with interrupts masked for a user-chosen length up to the full IPC buffer; cap the per-call length or re-enable interrupts around it.
    - `ipc_buffer::kernel_at` indexes `m_frames[]` with no bound, safe only because its one caller checks `contains()` first; assert the invariant in the function.
    - `dispatch_handle_op` takes the table lock in `verify` and again in each handler, re-resolving the id, so verify-then-execute is not actually held across one lock; and `unpack(handle)` is computed twice.
    - A partial `grow()` failure returns after appending some entries without chaining them, orphaning those slots permanently.
    - `create_handle` uses the free-list head without asserting `grow()` actually produced a slot; `-1` would index as `SIZE_MAX`.
    - `TypeRegistry` writes take `m_lock` but `lookup`, `lookup_by_name`, `count`, and `index_for_id` read unlocked, including on the handle-creation path. Either lock the readers or seal the registry after boot.
    - The rights argument is truncated from 64 to 32 bits without rejecting a nonzero upper half; `a2..a5` traverse the whole ABI unvalidated and discarded.
    - An unknown syscall number returns raw `-1` while an unknown handle op returns `invalid_operation`; pick one.
    - Dead: `TypeDescriptor::default_rights` (written by every registration, read by none), `TypeRegistry::lookup_by_name`, `HandleTable::info`, `HandleTable::is_valid`, the `break` after the `[[noreturn]]` `exit_current()`, and `insert()` as a pure forwarder to `create_handle()`.
- Enable SMAP/SMEP on x86_64 and leave `sstatus.SUM` clear on riscv64, so a stray kernel dereference of a user address traps instead of succeeding. The kernel never intentionally reads user mappings -- the ELF loader and the IPC buffer both go through the physmap -- so nothing needs an access window today, which makes this cheap to turn on and a real backstop if something later reaches for a user pointer by mistake.
- Replace the x86_64 syscall entry's single-core stack globals with per-CPU GS state when SMP scheduling lands.

## Task & Thread Lifecycle
- Implement task-kill and exception propagation (task/thread vocabulary per `docs/Design/Task Model.md` -- no processes, no UNIX signals).
- Extract the userspace runtime once a second user program exists. `src/sys/init/` currently owns `_start` (per arch), the syscall wrappers, the linker script, and its freestanding compile flags; each exists once, so factoring now would build a library with one caller. The second program is the trigger, and the thing to extract is a C runtime -- `_start`, syscall stubs, linker script -- as a package installing headers and a static archive next to `sys/kernel-headers`, not a libc. Nothing needs malloc, stdio, string, or locale, and naming it `libc` invites someone to supply them. Initrd will likely reshape userspace anyway, so committing late is cheaper than committing now.
- ELF loader follow-ups (static ET_EXEC for the running architecture is what loads today):
    - No `ET_DYN`/PIE support, which is the prerequisite for user-space ASLR; relocation processing is a milestone of its own.
    - Segments must be page-aligned and may not share a page. Ordinary lld output satisfies this, but a packed binary from another toolchain is rejected rather than mapped.
    - `MAX_SEGMENTS` is a fixed 8, chosen for static binaries; a real toolchain image with more loadable segments would be refused.
    - The user stack address and size are still fixed constants chosen by the kernel, not derived from the image; first-fit virtual address search is a VMM to-do.
    - `PT_GNU_STACK` is ignored -- the stack is mapped `READ|WRITE` unconditionally.
    - `e_phentsize` is accepted at any value at or above `sizeof(Elf64_Phdr)`, but only entry 0's alignment is checked, so a stride that is not a multiple of 8 misaligns every later header -- the UB the existing alignment check exists to prevent, and invisible to ASan. Require the stride to be a multiple of the alignment.
    - `entry_covered` ignores segment flags, so an entry point inside a non-executable segment parses clean and faults on the first instruction fetch.
    - A `PT_LOAD` without `PF_R` is accepted; on x86_64 write-without-read cannot be encoded, so it maps readable, wider than requested.
    - `i * e_phentsize` in the header walk is `int` arithmetic that can sign-overflow; reachable only past 2 GiB of image, so theoretical today.
    - A segment with `p_filesz > 0` and `p_memsz == 0` is skipped before `check_segment` sees it, so a malformed shape is ignored rather than rejected.
    - `symbols.cpp` bounds-checks with the declared `e_shentsize`/`sh_entsize` but walks the arrays at `sizeof` stride, so any larger declared entry size silently misparses every entry after the first. It also truncates `st_size` to 32 bits and can wrap the extent test in `find_entry`.
    - `elf_parse.cpp` and `elf_loader.cpp` each define `PAGE_SIZE` and hand-roll the same page round-up; share one helper.
- Supply debug metadata for user-mode stack unwinding and cooperative crash reporting (kernel-side crash reporting already exists).

## IPC & Services
- Handle-transfer gaps: no per-handle transfer right yet (no object type registers TRANSFER; the channel-handle gate is the only check), a receiver-table insert failure on dequeue closes the arrived handle rather than failing the recv (the message is already dequeued), and an endpoint escrowed on its own pair's queue is an unreclaimable reference cycle (the exact self-channel case is refused; the peer-through-itself shape is not detectable cheaply).
- Port gaps: one global lock for the whole subsystem with a non-IRQ guard (split it when contention shows; switch to the IRQ guard before interrupt objects signal from handlers), a forgotten binding pins its object forever (strong refs by design -- weak bindings with a closure packet are the upgrade), and packets carry no server-defined payload yet.
- Channel follow-ups toward the full `docs/Design/IPC Primitives.md` design: server dispatch / capability-aware routing, and per-task quotas replacing the fixed `MAX_MESSAGE_BYTES`/`QUEUE_DEPTH` caps (message storage is already page-per-message from the PMM -- `mm/channel_pages.cpp` -- so a quota can count pages, the same currency as the IPC buffers). The channel syscalls are hand-dispatched in `syscall.cpp` because they carry up to five args and touch the IPC buffer; fold them into the declarative op table when it learns both.
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
- Implement storage (AHCI or NVMe), RTC, entropy, and watchdog timer drivers. Wall-clock time already comes from the Limine date-at-boot request (`boot_info::boot_epoch_seconds`, shell `date`); an RTC driver is still wanted for non-Limine boot paths and for re-syncing drift on long uptimes.

## Security & Reliability
- Enforce memory zeroisation, W^X policies, and static analysis for privileged code paths.
- Integrate boot-time integrity checks for packages and kernel binaries.
- Add watchdog firing (the crash trigger enum slot is already reserved) and structured fault isolation reporting; assertion escalation policy already exists.

## Testing & QA
- Continue growing driver/core unit and stress coverage where gaps remain; hosted suites cover channels, ports, arenas, and handle tables, and syscall-level coverage rides in the init program -- add dedicated IPC stress/fuzz suites as the subsystem grows (multi-threaded port producers, transfer under pressure).
- VMM test gaps: WRITE_COMBINING end-to-end (indistinguishable from DEVICE until PAT lands) and OOM paths in the fault/commit paths (needs PMM fault injection).
- Wire up a CI pipeline (no config exists yet) that runs the existing host+QEMU tiers and applies the coverage gate -- coverage tracking and QEMU test automation are already done.
- Extend the fuzz harness to memory-subsystem interfaces now; add scheduler fuzz targets (run-queue rotation, wait-queue bookkeeping, signal-mask matching) now that the scheduler exists, and syscall fuzz targets once syscalls exist.
- Harness protocol lines can interleave with concurrent log flush output (one test_end line was garbled in the 2026-06-10 run, test still counted); make @@HARNESS emission atomic with respect to log flushes.
- Expand targeted coverage for: `core/cxx.cpp`, `core/interrupts.cpp`, `core/log.cpp`, `core/panic.cpp`, `core/time.cpp`.
- KTL edge-case gaps: self-move assignment (vector/ref/Result), ref refcount-overflow panic path, negative-compilation checks for deleted overloads (e.g. maybe<T&> rvalue binding).
- Harness assertion messages are interpolated raw into the `@@HARNESS` JSON, so any assertion text containing a quote or backslash corrupts the stream; route them through the existing `kernel::write_json_escaped`.
- Fuzz coverage gaps found in the post-Milestone-1 review: `elf_symbols_fuzz` stops at `locate_symbol_tables` and never reaches the per-symbol string handling in `init()`, which is where the bounds checks live; `elf_loader_fuzz`'s oracle asserts only `filesz <= memsz` and the file-byte read, not page alignment, the W+X rejection, extent wrap, or entry coverage.
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
