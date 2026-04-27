# Memory Subsystem

This page documents the current allocators and the planned virtual memory direction.
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
Until the scheduler exists, the inline fallback in allocation is the only path that performs zeroing, and the dirty pool is also drained on demand.

The PMM only counts reserved pages -- it does not own a list of reserved physical ranges.
Tracking specific kernel-occupied ranges belongs to the VMM, which receives them at init separately from the PMM's free-page accounting.

## Planned Architecture
### Virtual Memory Manager
The VMM is designed but not yet implemented.

**Priorities**: isolation > simplicity > latency > throughput.

The VMM is organized around five core objects: address spaces, regions, virtual memory objects (VMOs), pages, and pagers.

An address space is part of a [[Task Model|task]], not a separate kernel object.
There is no address space handle -- a task can only map VMOs into its own address space.
If task A wants task B to access shared memory, it sends a VMO handle through a [[IPC Primitives#Channels|channel]] and task B maps it itself.
The kernel provides no coherence guarantees on shared VMOs beyond what the hardware gives (cache coherence on x86_64).
Synchronization of shared memory is entirely the responsibility of userspace.

An address space represents a task's page table and contains a tree of regions that define its virtual memory layout.
Regions are nestable containers that own a virtual address interval.
They hold child regions and VMO bindings.
Child regions cannot overlap siblings or exceed parent permissions.

A VMO is a region of memory backed by a pager source.
It tracks resident pages, size, and statistics.
Pages are physical frames with lifecycle states ranging from wired through active, inactive, free, and zeroed.

Pagers are kernel-only policy objects that load or flush pages.
Three types are planned: anonymous (zero-fill), file-backed, and device (MMIO with cache attributes).

**Page replacement** uses a clock algorithm.
Reclaim pulls from inactive pages -- clean pages go to free, dirty file-backed pages write back first.
A background zeroing worker maintains a zeroed watermark.

**Fault handling** follows the sequence: trap, region lookup, authorization, resident check, pager fill, install PTE.
Clean unread pages map to a global read-only zero page; the first write triggers copy-on-write allocation.

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
