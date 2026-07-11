# Memory Subsystem

This page documents the kernel's allocators and virtual memory manager.
The kernel has two memory allocators today: an early heap for boot-time allocation and a Physical Memory Manager, also known as the PMM, for page-granularity allocation.

## Early Heap
A bump allocator that provides memory before the page allocator is available.
It is initialized during [[Boot Process#1. Early Boot|early boot]] with a dedicated region in BSS.

The early heap maintains a linked list of blocks, each marked as free or allocated.
On allocation, it walks the list for a free block large enough, splitting it if necessary.
On free, it coalesces adjacent free blocks.
`operator new` and `operator delete` delegate to the early heap.

## Physical Memory Manager
The PMM manages physical page frames.
Pages are 4K (`KERNEL_MINIMUM_PAGE_SIZE`).

The PMM is a free-pool manager with two pools: zeroed and dirty.
Allocation pops from the zeroed pool; if the pool is empty, the PMM zeroes one page inline as a fallback.
Free pushes the returned page to the dirty pool without scrubbing it.
The PMM never hands out a dirty page.

A background zeroing worker is the intended steady-state mechanism for moving pages from dirty to zeroed.
Until the background zeroing worker is built, the inline fallback in allocation remains the only path that performs zeroing.
The dirty pool is also drained on demand.

The PMM only counts reserved pages -- it does not own a list of reserved physical ranges.
Tracking specific kernel-occupied ranges belongs to the VMM, which receives them at init separately from the PMM's free-page accounting.

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
VMOs are resizable: growth extends the residency index lazily, and shrinking wins over mappings -- the tail is unmapped from every address space through the mapping back-references, its frames are freed, and later access past the new size faults to an error.
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

### Slab Allocator and UMI
The planned Unified Memory Interface (UMI) provides slab-based allocation for kernel objects, with per-type arenas and zero-on-free semantics.
Allocation and deallocation reduce to bitmask operations.
Not yet implemented.
