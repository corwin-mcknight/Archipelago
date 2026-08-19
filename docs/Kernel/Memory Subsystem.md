# Memory Subsystem

This page documents the kernel's allocators and virtual memory manager.
The kernel has three memory allocators today: an early heap for boot-time allocation, a Physical Memory Manager -- also known as the PMM -- for page-granularity allocation, and a slab heap that serves general allocation once the PMM exists.

## Early Heap
A block allocator that provides memory before the page allocator is available.
It is initialized during [[Boot Process#1. Early Boot|early boot]] with a dedicated region in BSS, ahead of the global constructors, because constructing a global may itself allocate.

The early heap maintains a linked list of blocks, each marked as free or allocated.
On allocation, it walks the list for a free block large enough, splitting it if necessary.
On free, it coalesces adjacent free blocks.
Exhaustion panics: nothing in the boot window can continue without memory.

`operator new` and `operator delete` delegate to the early heap until memory initialization activates the slab heap.
After that point allocation goes to the slab heap and exhaustion becomes a returned failure rather than a panic.
Deallocation stays correct across the switch because the early heap owns a known address range, so a pointer from before the switch is recognized by address.

## Physical Memory Manager
The PMM manages physical page frames.
Pages are 4K (`KERNEL_MINIMUM_PAGE_SIZE`).

The PMM is a free-pool manager with two pools: zeroed and dirty.
Allocation pops from the zeroed pool; if the pool is empty, the PMM zeroes one page inline as a fallback.
Free pushes the returned page to the dirty pool without scrubbing it.
The PMM never hands out a dirty page.

A background zeroing thread, started at [[Boot Process#5. Kernel Entry|kernel entry]], is the steady-state mechanism for moving pages from dirty to zeroed.
It works in small paced batches so that the initial climb to a zeroed supply trickles out instead of monopolizing the CPU.
Dirty pages always drain into the zeroed pool first; once none remain, the worker pre-zeroes untouched region tails in place until all free memory is zeroed.
The inline fallback in allocation is what covers the gap when the worker has not kept up.

The PMM only counts reserved pages -- it does not own a list of reserved physical ranges.
Tracking specific kernel-occupied ranges belongs to the VMM, which receives them at init separately from the PMM's free-page accounting.

## Slab Heap
The general-purpose kernel heap, backing `operator new` from memory initialization onward.
It is built from PMM pages addressed through the direct map, and it sits directly above the PMM: the PMM must never allocate through the heap, or a page acquired inside the heap's own lock would re-enter the allocator beneath it.

Requests up to a kilobyte are served from a small set of power-of-two size classes, one page per slab, with a tagged header at the front of the page and the slots following it.
Freed slots are threaded through themselves, and slots never yet used are tracked by a bump cursor, so a fresh slab initializes nothing beyond its header.
Larger requests skip the size classes entirely and take whole pages from the PMM.
Those runs carry no in-page header -- their length is recorded in the first frame's page descriptor -- which is what makes a page-sized request cost exactly one page, and what lets a free classify a pointer by its alignment alone.

Two rules make the heap safe rather than merely small.
Allocation from interrupt context is forbidden outright, checked at runtime, not compiled out; handlers reserve what they need when they are bound.
That single rule is what lets the heap run without reserved pools or interrupt-safe locking.
The heap lock is also a leaf: pages are acquired from and returned to the PMM outside it, so holding any other lock while allocating cannot form a cycle through the heap.

Corruption is detected rather than propagated.
A per-slab bitmap of live slots turns a double free, a free of a slot never allocated, an interior pointer, and a pointer the heap never produced into deterministic panics instead of freelist corruption, and large runs are authenticated through their page descriptor tag under the heap lock so racing frees of one run cannot both succeed.

## Architecture
### Virtual Memory Manager
The VMM described here is implemented for the kernel address space: the arch paging boundary, page descriptors, region tree, VMOs with anonymous and device pagers, demand paging with zero-page copy-on-write, and VMO resize.
Task-owned address spaces, region handles, and the userspace pager protocol arrive with the task and IPC milestones.

**Priorities**: isolation > simplicity > latency > throughput.

The VMM is organized around five core objects: address spaces, regions, virtual memory objects (VMOs), pages, and pagers.

**Architecture boundary**: the address space is one class completed by each architecture.
Its portable half (region tree, fault accounting, lifecycle) is common code; its paging half (map, unmap, walk, activate) and the shape of its embedded arch state are supplied by the architecture.
The paging interface speaks arch-neutral permissions (read, write, execute, user) and cache modes; each architecture translates these to its own page-table entry format internally.
Everything above it -- regions, VMOs, pagers, and page descriptors -- is portable code shared by all targets (x86_64 and riscv64).
Page-table entries carry no software-defined state: they are a cache of VMO and region truth, and the fault handler derives intent (such as copy-on-write) from the owning structures, not from spare PTE bits.

**Cache modes**: cached (normal memory), device (MMIO), and write-combining (framebuffers).
Modes are requests, not guarantees: an architecture may degrade a mapping toward stricter caching (write-combining to uncached) but never looser.
This accommodates riscv hardware without page-based memory types, where attributes come from fixed physical memory ranges.

An address space is part of a [[Task Model|task]], not a separate kernel object.
There is no address space handle -- a task can only map VMOs into its own address space.
The one planned exception is region delegation, described below.
If task A wants task B to access shared memory, it sends a VMO handle through a [[IPC Primitives#Channels|channel]] and task B maps it itself.
The kernel provides no coherence guarantees on shared VMOs beyond what the hardware gives (cache coherence on x86_64).
Synchronization of shared memory is entirely the responsibility of userspace.

An address space pairs the arch page-table object with a tree of regions that define its virtual memory layout, plus fault counters.
A kernel address space exists from VMM initialization and receives the kernel's wired physical ranges at init, separate from the PMM's free-page accounting.

Regions are nestable containers that own a virtual address interval.
They hold child regions and VMO bindings; children are kept in a balanced tree ordered by base address.
Child regions cannot overlap siblings or exceed parent permissions.
Regions are reference-counted kernel objects from the start, shaped for eventual handle exposure: handing out a region handle will let the holder map into that interval of the owning task's address space.
Handle exposure, the detached-region state machine, and delegation semantics arrive with the task and IPC milestone; until then regions are reachable only from kernel code.

A VMO is a range of memory backed by a pager source.
It tracks resident pages, size, statistics, and back-references to every mapping of it.
VMOs are fixed-size; the back-references exist so eviction and writeback can find every translation of a page when those land.
Residency is tracked in a chunked index whose chunks are whole page frames allocated directly from the PMM, arriving pre-zeroed from the zeroed pool.

Pages are physical frames with lifecycle states ranging from wired through active, inactive, free, and zeroed.
Address holes, firmware ranges, and device windows outside RAM carry a separate MMIO state so they never appear in memory usage accounting.
Per-frame state -- lifecycle, share count for copy-on-write, owner back-reference -- lives in a global page descriptor array indexed by frame number, allocated at VMM initialization to cover usable RAM.

Pagers are kernel policy objects that load or flush pages.
Two kernel pagers ship first: anonymous (zero-fill) and device (MMIO ranges with cache attributes, never evictable).
File-backed memory is not a kernel pager: filesystems are userspace servers, so file backing arrives later as a userspace pager protocol over channels, slotting in behind the same per-page fill and writeback interface.

**Page replacement** applies only to pager-backed evictable pages and arrives with the userspace pager milestone, using a clock algorithm over active and inactive pages -- clean pages go to free, dirty pages write back through their pager first.
Anonymous memory is never swapped: it is RAM-resident by design, so secrets never reach disk.
Until evictable pages exist, memory exhaustion surfaces as an allocation failure returned to the caller.
A background zeroing worker maintains a zeroed watermark.

**Fault handling** follows the sequence: trap, region lookup, authorization, resident check, pager fill, install PTE.
Clean unread pages map to a global read-only zero page -- a single wired frame allocated at VMM initialization -- and the first write triggers copy-on-write allocation.

**Locking** starts as a single kernel-wide VMM lock covering region trees, residency, and descriptors, taken by the fault handler as well.
Splitting into per-address-space and per-VMO locks is deferred until scheduler-era contention is measurable.

**Security**: W^X enforcement, SMEP/SMAP.
Kernel mappings are never visible to user mode.
The Higher-Half Direct Map (HHDM) provides a full-RAM direct map in the kernel's higher half.

**Observability**: Per-address-space fault counters, per-VMO residency stats, debug dumps printable to serial.

### Synchronization Primitives
The kernel provides two classes of synchronization for shared memory coordination:

**Handle-based sync objects** -- kernel-defined mutex, semaphore, and related types accessed through handles.
These follow the same [[Object Model|object model]] and [[Object Model#Three-Path Dispatch|dispatch pipeline]] as any other kernel object.
Suitable for coarse-grained cross-task synchronization where syscall overhead is acceptable.

**Futex-on-VMO-offset** -- kernel-assisted wait/wake on a specific offset within a VMO.
A task atomically checks a value at a VMO offset and sleeps if the value is not what it expects.
Another task writes the value and wakes waiters.
Scoping futexes to a VMO offset rather than a raw virtual address keeps them within the capability model -- you need a handle to the VMO to wait on it.
Suitable for performance-critical shared memory coordination where syscall overhead matters.

### Unified Memory Interface
The size-class layer described above is the first phase of the planned Unified Memory Interface (UMI).
The per-type arena phase is implemented: named object caches of exact-size slots over the same page seam, one per client type, listed by the shell's `mem` command.
An arena's slots are always zero -- free scrubs the slot immediately, fresh slabs arrive zeroed -- so allocation and deallocation reduce to bitmap operations with no freelist threaded through the memory, and freed objects' contents never linger.
Arenas enforce the same rules as the heap (no interrupt-context allocation, leaf locking, deterministic panics on bad frees) and currently serve Thread, channel state, and port bindings.
The remaining phases are allocation hardening (poisoning, redzones, a guard-page debug mode) and per-CPU magazines once SMP scheduling lands.
