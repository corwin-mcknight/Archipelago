# Memory Management

This document describes the virtual memory subsystem of the Arichpelago kernel. It defines the core structures and interfaces that end up creating each task's view of memory. The VMM controls access to memory via permissions, handles, and pagers. Heap allocators are out of scope; they sit on top of this VM layer.

## Goals and Constraints
- Priorities: isolation -> simplicity -> latency -> throughput.
- Kernel is mapped into every address space.
- Currently only handles a single page size (4 or 16 KiB).
- All metadata (page tables, region trees, pager state) is considered wired memory.
- Pagers: kernel-only (ram_pager, file_pager, optional device_pager). Any pager failure is a panic.
- Direct map: full-RAM kernel-only physmap; never visible to user mode.
- Security: W^X, SMEP/SMAP/PXN; user cannot read kernel mappings.

## Core Objects

### vm_address_space (Address Space)
Represents a task's page-table. It contains a tree of vm_region nodes that together define the task's virtual memory layout.

### vm_region (Region)
A nestable container that shapes the address space. A region owns a virtual address interval and can contain:

- Child regions (non-overlapping, fully contained)
- A collection of VMO bindings that together cover some subset of the region's range

However, a vm_region cannot:
- Overlap with other sibling or ancestor regions.
- Have higher permissions than its parent.

### vm_object (VMO)
A region of memory backed by some source. Those sources are `vm_pager`. 
They reference their pager, what this object represents, how big it is in memory, where it starts in memory, statistics, and what pages are currently resident.

### vm_page
A physical frame tracked by the physical memory manager.

- State machine: WIRED -> ACTIVE <-> INACTIVE -> FREE -> ZEROED.
- Metadata: dirty bit, last access stamp, pager-specific data (for example, file offset).

### vm_pager
A kernel-only policy object that loads or flushes pages for a VMO.

- ram_pager: anonymous, zero-fill-on-demand.
- file_pager: page-in or page-out file ranges.
- device_pager (optional in v1): map MMIO with cache attributes (uncached, write-combining, and so on).


## Regions, Bindings, and the View of Memory
A region's view is the composite of its bindings:

- Binding components: (vaddr_start, length, vmo, vmo_offset, prot, flags)
- A region may hold many bindings that together shape the process-visible bytes in that interval.
- Bindings must not overlap within a region. Child regions may cover subranges and override bindings beneath them, subject to the monotonic permission rule.

The view of memory via a region is the combination of all memory objects it contains. The lowest set of permissions along a path apply. 


## Physical Memory Manager

The physical memory manager manages lists of pages. It stores these within a `vm_page` structure that stracks state and metadata. Pages that are truely free are not actually tracked as `vm_page` structures; there is simply a compact `vm_page_region` structure that tracks a contiguous run of free pages.

- Pages can be wired, active, inactive, free, or zeroed.
- Pages that are wired are in use by the kernel or pinned by user processes. They do not participate in page replacement.
- Active pages have been recently used.
- Inactive pages have not been used recently and are candidates for reclamation.
- Free pages are available for allocation.
- Zeroed pages are free pages that have been zeroed and are ready for immediate use.

The physical memory manager will never hand out a dirty page. Pages will be zeroed before being given to a user process. Free pages may be dirty, but they will be cleaned before being handed out. Zeroed pages are preferred first.
A background process will maintain a pool of zeroed pages by zeroing free pages in the background.

### Page Replacement Policy
A simple clock algorithm with three lists:
- Reclaim:
  - Pull from INACTIVE.
  - If clean, move to FREE.
  - If dirty and file-backed, perform synchronous write-back, then move to FREE. Anonymous pages free immediately.
- Zeroer: a background worker maintains a ZEROED watermark by zeroing from FREE.


## Fault Handling
1. Trap and classify: access type (read, write, execute) and privilege level (user or kernel).
2. Region lookup: walk the AS region tree to find the lowest region covering the faulting virtual address; select the effective binding (child overrides parent).
3. Authorization: verify the region handle and, for private VMOs, the VMO handle in the current process; check that permissions allow the access.
4. Resident check: consult the VMO's resident set.
   - If present: install the PTE (respect W^X) and mark the page ACTIVE.
   - If absent: call the VMO's kernel pager to fill; any error triggers a panic.
5. Return and update per-AS and per-VMO counters.

Zero page handling: clean, unread pages map to the global read-only zero page; the first write allocates a private page from ram_pager.

No-fault rule: the fault path never uses pageable memory. All metadata (AS, region, binding, VMO, page, pager state) comes from wired slabs.


## Kernel Mapping and Direct Map
- Kernel text and data, along with core structures, are globally mapped into all address spaces and marked global in PTEs (PCID/ASID).
- A full RAM direct map (physmap) is present in the kernel's higher half and is never exposed to user mode.


## Observability
- Per address space: minor and major faults, wired pages, reclaim events, shootdowns received.
- Per VMO: resident pages, pager calls, last error if any.
- System-wide: zeroed watermark breaches, reclaim rate, reserve dips.
- Debug views:
  - as_dump(asid) prints the region tree and bindings (with coverage and permissions).
  - vmo_dump(vmo_id) prints residency ranges and pager stats.
  - All dumps are printable to serial early in boot.
