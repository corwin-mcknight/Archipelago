# Development Backlog
The work ahead for Archipelago: known correctness gaps, the next usable system features, and the infrastructure needed to support them. [MILESTONES.md](MILESTONES.md) describes the larger outcomes; this backlog identifies the changes and dependencies that move the implementation toward them.

Read the numbered sections as a suggested order of investment. Correctness fixes come first, followed by feature slices and their prerequisites. Testing, tooling, and focused cleanup should accompany the work they support; independent tasks need not wait for an earlier section to be finished.

Deferred items name the consumer or condition that would justify taking them on. Accepted constraints record deliberate boundaries to preserve when implementing related work. Keep active entries concrete: describe what remains, name any prerequisite, and remove them when complete.

## 1. Correctness & Hardening
These findings remain visible in the source; this is a source review, not a fresh reproduction of every failure. Add regression coverage with each fix, using the host or freestanding lane appropriate to the code.

### Memory ownership and allocation failures
- Validate PMM frees for alignment and eligible frame state (`mm/pmm.cpp`). FREE/ZEROED double frees are already rejected when descriptors cover the frame; WIRED/MMIO and unaligned frees still need protection.
- Reject kernel-half mutations through user address spaces in `mm/paging.cpp`. `map_page`/`unmap_page` currently permit canonical kernel addresses, whose intermediate tables are shared; define a separate kernel-mapping path before adding dynamic kernel mappings.
- Make VMO construction report chunk-index allocation failure (`mm/vmo.cpp`); it currently discards `m_chunks.push_back` failure and can advertise more pages than the index covers.
- Define device-VMO reservation ownership and rollback (`mm/pager_device.cpp`). `create_device_vmo` marks frames WIRED before VMO allocation succeeds and never restores the reservation on destruction; account for overlapping windows before simply unmarking them.
- Audit fallible allocation callers, including `ktl::make_ref`, now that nothrow allocation can return null after `heap_activate()`. Ordinary `operator new` and the pre-PMM early heap still panic on exhaustion.
- Handle task-list allocation failure in `task/task.cpp::register_task` instead of discarding it.
- Protect the early-heap block list across CPUs, or prove/enforce that all remaining accesses are boot-core-only. Allocation switches to the slab before AP startup, but later frees of early pointers and statistics still enter `early_heap`; any lock must be constant-initialised for pre-constructor use.
- Validate boot memmap overlaps before admitting USABLE ranges to the PMM (`core/boot.cpp`). Wrapping ranges and descriptor-cap exclusions are already handled; conflicting usable/reserved or kernel-image ranges remain a gap.

### ELF validation
- In `elf/elf_parse.cpp`, validate every program-header alignment, including the `e_phentsize` stride; cast before `i * e_phentsize` to avoid promoted signed multiplication overflow.
- Require the entry point to fall in an executable segment, and reject `p_filesz > 0 && p_memsz == 0` before skipping empty segments.
- Define/reject unrepresentable `PT_LOAD` permissions: without `PF_R`, x86 mappings become readable anyway, and riscv64 write-only PTEs are invalid.
- Fix symbol-table stride validation/walking in `crash/symbols.cpp`: declared `e_shentsize`/`sh_entsize` and actual `sizeof` strides disagree. Avoid truncating `st_size` to 32 bits and wrapping the `find_entry` extent check.

### Traps, stacks, and boot
- Clear DF in x86 `trap_sp_overflow` before entering C++ (`x86_64/interrupt_handlers.s`); the branch bypasses the normal entry's `cld`.
- Resolve the stack-publication window in `task/scheduler.cpp::switch_to`: the incoming stack floor and TSS/syscall stack are published while still on the outgoing stack. Move publication to an appropriate incoming-stack hook, including first-run paths. Clear/poison the syscall stack for stackless threads instead of retaining the previous value.
- Reconcile `enable_nxe()` ordering and its contract (`x86_64/main.cpp`): boot memory setup activates copied NX mappings before this helper runs, relying on Limine's NXE state.
- Make boot CPU accessor bounds checks survive NDEBUG (`boot/limine/limine_boot.cpp`, `cpu_hw_id`/`start_cpu`).
- Move `.init_array` into the read-only PHDR in both linker scripts and explain the extra `.bss` padding. Review GDT alignment explicitly; its packed layout currently has no requested alignment (an audit item, not a demonstrated boot failure).
- Enable feature-detected SMAP/SMEP on x86_64; explicitly establish and verify clear `sstatus.SUM` on riscv64. ELF loading and IPC buffers use physical mappings, so current user-memory access needs no temporary access window.
- Add VMM-mapped guard pages to kernel stacks after the kernel-mapping path is defined. Add x86 IST-backed exception/NMI stacks for stack-overflow and double-fault reporting; the current emergency-stack tripwire is already implemented.

### Syscalls and IPC boundaries
- Seal `TypeRegistry` after boot or synchronise readers with registration; `lookup`, `count`, and `index_for_id` currently read without the writer lock.
- Validate upper rights bits in `SYS_HANDLE_DUPLICATE`; audit `SYS_HANDLE_RESTRICT` REMOVE mode's truncation separately from RETAIN, which already rejects values above UINT32_MAX. Define reserved-argument handling per syscall; arguments a2..a4 are used by the current ABI, while a5 is ignored.
- Bound the interrupt-masked work in `syscalls/debug.cpp::sys_write`. Before user threads run on APs, also serialise or replace its shared line buffer.
- Define receive-side handle insertion failure semantics (`syscalls/channel.cpp`): a message is already dequeued when insertion fails, and the undeliverable handle is closed.
- Define whether transferred objects need their own transfer permission. Today RIGHT_TRANSFER gates the sending channel; it is not checked on each carried handle. Both endpoints of the sending pair are already rejected from their own queue; audit ownership cycles spanning multiple pairs separately.
- Count/trace a spawn only after successful enqueue (`task/spawn.cpp::thread_enqueue`); failure currently unwinds the thread after recording a spawn.

## 2. Milestone 2 -- Useful Work in Userspace
[Milestone 2](MILESTONES.md) establishes the complete boot -> userspace shell -> program writes file -> separate program reads file path. The kernel loads only init and hands it the opaque initrd; file and path semantics belong to userspace.

### Task construction and userspace loading
- Expose primitives to create an unstarted task, populate its address space through authorised region handles, prepare its first thread and bootstrap endowments, and start it. Define cleanup for abandoned or failed construction.
- Enable authorised executable mappings with kernel-enforced executable-memory protections, including writable aliases, so init can load programs without the kernel interpreting their ELF images.
- Add init's userspace ELF loader and move subsequent program launches onto task-building primitives; retain the kernel ELF loader for the initial init executable.

### Bootstrap and file services
- Build an initrd containing the bootstrap servers, shell, programs, and data files. Supply init and the initrd as the two userspace Limine boot inputs.
- Define the initrd format and bootstrap-server discovery convention. Give init a minimal reader so it can find and launch the initial servers before file access exists.
- Implement the userspace file server's file/directory protocol and namespace: writable anonymous storage at `/`, with the read-only initrd exposed at `/boot`. Anonymous storage is a feature of the file server and lasts for the current boot.
- Define capability-based file access and executable-content delivery, including lifetimes and failure behaviour. The shell obtains program contents from the file server and asks init to execute them.

### Interactive shell and completion demonstration
- Load the userspace shell through file access once the file server is ready. Support browsing and reading files, program arguments, launch requests to init, and observing completion.
- Supply interactive input and standard streams. Demonstrate one program creating and writing an anonymous file and a separately launched program reading and printing it on x86_64 and riscv64.
- Cover read-only `/boot`, absent files, invalid executables, and returning to a usable shell after program exit. Verify anonymous files disappear on reboot.

### Standard streams
Implement [standard streams](docs/Design/Standard%20Streams.md) as part of the interactive environment. Sockets, ports, channel transfer, and atomic in-place rights restriction already exist.

1. Define the stdio bootstrap message and runtime consumption before program entry. Wire output/error socket ends with narrowed rights; leave input explicitly absent until a source exists.
2. Add the console server, multiplexing readable socket ends through a port and draining to debug write initially.
3. Wire coordinator endowment immediately after spawn, including parking read ends until the console server arrives. Cover spawn-order independence, absent stdin, peer closure, backpressure, and cleanup on partial failure.
4. Move ordinary program output to endowed streams. Device ownership follows the driver work below.

### Coordinator and lifecycle follow-ups
- Handle boot-module delivery beyond `Channel::QUEUE_DEPTH` (8) with chunking, draining, or retry; `endow_boot_modules` currently logs failed IMAGE delivery but does not retry.
- Replace or explicitly expose fixed coordinator limits as workloads grow: 8 children/registrations/parked connects, 31-byte service names, and echo's 4-client limit. Add negative replies or timeouts for connects to names that never register.
- Implement exception propagation and user crash-reporting/unwinding metadata, per [Task Model](docs/Design/Task%20Model.md). Task kill, exit status, TERMINATED, and child-death observation already exist.
- Define restart and capability-revocation policy for crashed servers. Retain/reload image VMOs for respawn; coordinator currently closes them after spawn. Include structured fault isolation reporting.
- Add per-program registration/connect policy with manifests once packaging exists; open-with-logging is the current intended policy.
- Clarify the remaining server dispatch/capability-routing work against [IPC Primitives](docs/Design/IPC%20Primitives.md) and [Service Coordination](docs/Design/Service%20Coordination.md); named register/connect routing has already landed.

## 3. Userspace SMP & Memory Coherence
Kernel threads already run on secondary cores on both architectures, with local timer ticks, reschedule IPIs, per-core idle threads, and the reaper's `on_cpu` handoff. User threads remain on the boot core.

- Implement acknowledged x86 TLB shootdown; its arch hook is still a no-op. riscv64 already implements SBI RFENCE remote invalidation. Validate shared-address-space activation/unmap races and frame-reuse ordering before allowing user threads on APs.
- Audit GLOBAL mappings when a space is inactive and paging-structure-cache invalidation when widening intermediate USER bits. Define invalidation for shared kernel mappings as part of the kernel-mapping work above.
- Finish the user-concurrency audit (debug output, shared syscall state, address-space and IPC lifetime), then remove boot-core affinity and test simultaneous user execution/teardown on both architectures. x86 syscall entry and stack floors already use GS-local state.
- Audit the remaining plain `cpu_core::lapic_id` bring-up accesses; make publication race-free if readers can overlap writes. GS identity itself is implemented.
- Extend JH7110's DTB-discovered PLIC from the boot-hart context to per-hart routing when distributing external IRQs. Software wake IPIs already use SBI; direct CLINT routing is only needed for a future non-SBI platform.
- Add per-CPU trace rings if the shared scheduler-locked ring becomes a bottleneck or needs CPU attribution. Per-core accounting already exists.
- Add per-core run queues/load balancing only if profiles justify replacing the shared FIFO and boot-core user queue; retain round-robin policy.

## 4. Device Ownership & Platform Support

### Milestone 3 -- Ethernet and IPv4 Networking in Userspace
- Establish a userspace network driver for a chosen Ethernet adapter, including discovery, resource delegation, interrupt delivery, and the DMA isolation/teardown contract required by the device.
- Add the userspace network stack and application-facing service for Ethernet, address resolution, IPv4, and ICMP. Define how the connection is configured, including the route to external IPv4 hosts.
- Have init start and endow the network services. Support stopping and restarting the driver, restoring connectivity while the shell and file services remain usable.
- Add a userspace ping program and demonstrate incoming echo replies and outgoing ping to a reachable internet IPv4 address. Use a controlled peer for repeatable regression coverage.
- Plan TCP and an HTTP server as subsequent work building on file and network services; they are outside Milestone 3's Ethernet/IPv4/ICMP completion scope.

### Platform and device prerequisites and follow-ups
- Read riscv64 timebase frequency and UART address from the DTB; boards still supply constants despite existing DTB/PLIC discovery.
- Add ACPI RSDP/MADT discovery and CPU topology diagnostics for x86, then IOAPIC device-interrupt routing. Limine MP startup, LAPIC timers, and PIC masking are already implemented.
- Add UART RX interrupts and a waitable input queue, replacing shell sleep-polling. Use IOAPIC on x86 and the existing PLIC path on JH7110; test burst/pasted input on real UART FIFOs.
- Before interrupt objects signal from handlers, make the port signalling path IRQ-safe (`obj/port.cpp` currently uses a non-IRQ critical guard). Split its global lock only when contention warrants it.
- Give kernel MMIO dedicated cache-correct mappings; `mmio_region` currently uses the bootloader HHDM. Add x86 PAT programming for real write-combining; riscv64 cache attributes need platform/PMA or Svpbmt support, not merely the stored software cache-mode tag.
- Expose authorised device-VMO mappings and interrupt objects to userspace, then move the console server to a userspace UART driver. Keep a raw panic fallback and define how kernel log output shares the device.
- Extend keyboard support to boards/devices that need it (notably JH7110); x86 polled PS/2 input already feeds the console. Add interrupt-driven input with the routing work above.
- Extend the framebuffer terminal with UTF-8 decoding/font fallback, glyph caching if measured useful, and scrollback. The terminal and x86 keyboard path already exist.
- Improve pre-console panic output and synchronisation of the UART health flag; writes before UART init are currently dropped.
- Add deadline-driven timer events and a deadline queue for sub-tick sleeps/preemption: x86 LAPIC one-shot/TSC-deadline, and riscv64's existing one-shot SBI timer. High-resolution elapsed-time reads already exist.
- Add entropy and RTC drivers as consumers require them. Limine supplies date-at-boot today; RTC support is for other boot paths and long-uptime resynchronisation.
- Add crash watchdog injection/reporting and SMP crash fan-out through IPIs. JH7110 already arms a hardware watchdog; a reserved crash-trigger enum alone is not the missing hardware driver. Evaluate shell-drop on recoverable diagnostic crashes separately from fatal corruption.

## 5. Memory Lifecycle & Resource Policy
- Reclaim bootloader-reclaimable memory after copying live Limine data and retiring all boot/AP stacks that use it. It is currently excluded from PMM; the amount depends on the boot environment.
- Reclaim boot modules once no loader/VMO consumer references them. Use `boot_info::modules` to identify module extents (memmap classifies them with the kernel); return only safely owned page-aligned interiors. Add MODULE classification with the reclaim path if needed.
- Implement reusable contiguous free runs in PMM (buddy or equivalent). `alloc_contiguous` only carves untouched region tails; freed pages do not restore contiguous capacity, which limits large-heap allocation churn.
- Finish physical-address typing through PMM/VMM and retire `direct_map_address()` after pointer-identity clients have narrower APIs. Typed physical access and `mmio_region` have already landed.
- Add binding splitting for partial unmap; current unmaps cover whole slots.
- Define VMO sharing/duplication rights and lifetimes, then clone/share counts and shared-frame CoW beyond the zero page. Include cross-core coherence tests.
- Introduce per-task resource accounting/quotas across VMO memory, IPC buffers, channel message pages, sockets, and handles. Fixed per-buffer/queue limits do not bound a task's aggregate pinned memory.
- Define a userspace memory-pressure Event (low/critical levels), with hysteresis and an explicit global/per-task policy, so servers can release caches. Decide whether ignoring it has consequences.
- Harden object arenas with poisoning, redzones, and optional guard pages. Add per-CPU magazines when allocation contention warrants them; SMP kernel scheduling already exists.
- Move handle entries to arenas only after a deliberate chunked-table refactor; the current contiguous vector cannot directly consume independent arena slots.

## 6. Storage, Packaging & Release Security
Dependencies run from device access through storage to policy and distribution.

1. Add a block-device abstraction with asynchronous I/O and caching, and a first storage driver (AHCI or NVMe).
2. Define the package format/trust roots and implement the package store plus signed read-only root mount path. Integrate package/kernel boot integrity checks with the signing policy.
3. Add writable user storage with an explicit crash-recovery choice (journaling, snapshots, or rollback).
4. Define versioning, artifact signing/provenance, and release validation gates covering correctness, security, performance, and target compatibility.
5. Automate ISO/image publishing and mirroring once the signing and regression gates exist.

- Audit zeroisation and W^X end to end, especially new shared/device/executable mappings, and add privileged-code static analysis. PMM allocation already zeroes pages, the ELF parser rejects W+X segments, and user VMO syscalls currently expose no EXEC permission.
- Optional kASLR needs relocation/tooling verification. Userspace ASLR depends on ET_DYN/PIE and relocation support; the current loader accepts static ET_EXEC only.
- Publish contribution/security guidance and keep the existing roadmap current. Add a changelog/release communication process when releases have consumers; a generic stakeholder cadence is not a kernel implementation prerequisite.

### Milestone 4 -- Persistent storage and user accounts
- Establish persistent storage through userspace drivers and file services, then add userspace account, authentication, and session services with persistent personal files and settings. Establish this model before the desktop's application-launch and file-access contracts.
- Define authenticated capability grants, application-specific endowments, and administrative service access. Ensure init preserves the requesting session's authority limits and that file services enforce access independently of path knowledge or caller-supplied account IDs.
- Define explicit sharing and logout semantics, including task cleanup and invalidation of session-scoped grants; closing a handle alone does not revoke delegated copies.
- Demonstrate two isolated local accounts, explicit resource sharing, and logout/login without exposure of the previous session's private resources. Map POSIX identity/ownership interfaces onto trusted services as ported applications require them.

## 7. Continuous Validation & Developer Tools
Work here can accompany every feature slice; fix protocol reliability before relying on unattended CI results.

### Test reliability and coverage
- Escape assertion/reason strings in harness JSON using `kernel::write_json_escaped`. Console line locking already protects `ShellOutput::print` and `event` against concurrent output; retain interleaving regression coverage as logging evolves.
- Choose and wire one CI system to run host tests, the x86_64/riscv64 QEMU matrix (`plume test --arch all`), and the existing coverage gate. Keep real-board validation as a separate hardware lane; no CI config is currently checked in.
- Extend IPC stress/fuzz coverage to concurrent port producers, channel/socket backpressure, transfer under allocation pressure, and teardown. Hosted object suites and dedicated freestanding syscall tests already exist.
- Add memory-subsystem, scheduler/wait-queue/signal, and syscall fuzz targets; all of those subsystems exist now.
- Strengthen ELF fuzz oracles for alignment, W^X, wrapping extents, and executable entry coverage; drive symbol ingestion/string handling beyond `locate_symbol_tables`.
- Add PMM fault injection for VMM fault/commit OOM paths. Add WRITE_COMBINING end-to-end coverage after real cache-mode support lands.
- Grow riscv64 sfence/TLB and multi-source/per-hart IRQ coverage; architecture tests already include reschedule IPI coverage in addition to the JH7110 UART/PLIC test.
- Expand targeted tests for `core/cxx.cpp`, interrupt management, log rendering, `core/panic.cpp`, and uncovered time behaviour. Time and log-ring host tests already exist.
- Add focused GDT/IDT and LAPIC timer/core-init scenarios beyond current interrupt and SMP smoke coverage.
- Add KTL self-move assignment cases (vector/ref/result), refcount-overflow failure coverage, and negative-compilation checks for deleted overloads such as `maybe<T&>` rvalue binding.

### Build and debugging tools
- Include board `*.s` files in the kernel Makefile's platform source discovery, alongside `*.cpp`/`*.S`.
- Fix the Doxygen inputs to use existing sources and `docs/Kernel/`; its current `src/sys/kernel/docs/` inputs do not exist, and PROJECT_BRIEF still names only x86_64.
- Decide whether Plume should validate composed sysroot contents before imaging. Its stamp detects package changes, not external deletion/tampering; deleting the sysroot or its adjacent `.stamp` forces recomposition today.
- Document a concrete GDB/QEMU attach workflow (port, symbols, break-on-entry). Add ad-hoc tracing/log capture only where the test harness and existing serial-mux/board tools do not cover the need.
- Extend shell inspection with individual object/handle detail and memory/register/stack dump commands. `handle all` already dumps every task's handles; allocation and per-core scheduler metrics already exist. Add interrupt counts, timer diagnostics, latency percentiles, or richer scheduler views for specific debugging needs.
- Update stale current-state descriptions in `docs/Design/Scheduling.md` (still says no userspace and parked APs) and audit related syscall/task docs after the source split and SMP work.

## 8. KTL & Focused Cleanup
- Rework owning `maybe<T>` to explicit lifetime storage so empty values do not construct T and non-default-constructible types work.
- Add reference-returning vector accessors using the existing `maybe<T&>` specialisation; `at` and `back` currently copy, and `front` is absent.
- Make `vector::emplace_back` variadic in-place construction or remove/rename its forwarding-only alias.
- Add move-aware rvalue overloads to the remaining `maybe` combinators when a caller needs them. `result` no longer has the old monadic combinator surface; do not recreate it merely to satisfy the old audit.
- Review failure reporting for `register_interrupt` and `symbols::init`; `static_vector::push_back` already reports capacity failure with bool. Use results where a caller can act on distinct errors, rather than converting all void/bool APIs mechanically.
- Format log prefixes through `fixed_string`'s interface and prove the prefix capacity instead of writing `m_buffer` directly.
- Add minimum-severity filtering to the log pipeline; buffered sinks and crash-log emission already exist.
- Consolidate the common scheduler rotation in `yield`/`service_pending_preemption` if it improves clarity without hiding their different accounting and kill handling.
- Share ELF page-rounding helpers; use `KERNEL_MINIMUM_PAGE_SIZE` in descriptor `coverage_end`; consolidate duplicate EFER constants and active-root flush guards where the arch boundary stays clear.
- Apply CamelCase type / UPPER_SNAKE constant conventions to outliers as their code is touched; avoid a priority-displacing whole-tree rename.
- Recheck unused includes after the source split. Remaining deletion candidates include `TypeDescriptor::default_rights`, `assert_thread_context`, duplicate non-const `maybe::has_value`, and the test-only ctype shim. `HandleTable::is_valid` and public `insert` have production callers; `irq_depth` checks balanced exits. Tuple, the unused string-copy shims, unused bit/range/maybe helpers, `static_vector::peek_back`, `HandleTable::info`, and the duplicate synchronization semaphore have already been removed.

## Deferred Until a Consumer or Measured Need
- NUMA policy after topology discovery and multi-node hardware/workloads; reserved regions are already excluded from allocation, with overlap validation tracked above.
- Owned 2M/1G mappings after a concrete large-page consumer; current allocation/mapping uses 4K, while page walks already understand bootloader large leaves.
- User-pager eviction/clock replacement after a user-pager interface exists. Revisit ACTIVE versus WIRED page-table descriptors then; anonymous swap is intentionally out of scope.
- General VMO resize/protect/commit/decommit operations beyond the Milestone 2 loader's needs when growable arenas, guard pages, or pager consumers define the contracts. Region-based task construction and executable mappings are active Milestone 2 work above.
- Replace fixed IPC-buffer slots with `map_anywhere` when address-space layout needs it; any buffer eviction must invalidate the cached physical-frame contract.
- Generalise ELF segment packing, the eight-segment limit, stack layout, and PT_GNU_STACK policy when supported toolchains require it. Keep the default stack non-executable. Failed spawn parsing can leave image pages committed until VMO destruction; add rollback/decommit with reclaimable image VMOs.
- Optional weak port bindings with closure packets, and server-defined packet payloads, when a server needs them. Current strong bindings intentionally pin their objects.
- Allocate FPU save storage only for user threads when Thread footprint matters; the embedded area currently costs kernel threads too. Add teardown ownership with that change.
- Lift the host page-source stub's 4096 live-run cap if a stress workload reaches it.
- Add rb_tree predecessor/reverse iteration when a caller outgrows `find_le`.
- Factor the duplicated boot spine when another architecture exposes the real variation. Keep linker scripts architecture-owned unless a board needs a different load address.
- Standardise unknown syscall returns (currently raw -1) with a deliberate ABI compatibility decision. Publish object type IDs when a userspace consumer needs named constants.

## Accepted Constraints
These describe current policy, not unfinished implementation.

- Bootstrap channels are parent-to-task. Only the coordinator's parent end is held by the kernel; PEER_CLOSED observes parent death. A child can retain up to the bounded mailbox queue until teardown. A future kernel control plane needs its own explicit interface.
- Killed threads do not park again on signal waits after the kill scan. A killed thread contending on a kernel mutex may busy-wait with preemption until it reaches the syscall exit boundary.
- Anonymous memory is not swapped. Fallible allocation returns an error/null; boot allocation and ordinary `operator new` retain panic-on-OOM contracts.
- User VMO creation has no policy size cap until quotas exist; allocation may fail during metadata construction or later at commit/touch. Direct PMM clients also include kernel stacks, heap/arena backing, channel pages, and page tables -- VMM is not the sole PMM consumer and does not currently provide a general reclaim/retry policy.
