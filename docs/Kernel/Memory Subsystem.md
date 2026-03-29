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

Pages exist in one of three states: free (available but potentially dirty), active (in use), or zeroed (free and zeroed, ready for immediate use).
The PMM allocates from the zeroed pool first.
A background pass moves free pages to zeroed to maintain supply.
The PMM never hands out a dirty page.

## Planned Architecture
### Virtual Memory Manager
The VMM is designed but not yet implemented.

**Priorities**: isolation > simplicity > latency > throughput.

The VMM is organized around five core objects: address spaces, regions, virtual memory objects (VMOs), pages, and pagers.

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

### Slab Allocator and UMI
The planned Unified Memory Interface (UMI) provides slab-based allocation for kernel objects, with per-type arenas and zero-on-free semantics.
Allocation and deallocation reduce to bitmask operations.
Not yet implemented.
