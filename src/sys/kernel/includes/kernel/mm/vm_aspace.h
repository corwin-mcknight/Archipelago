#pragma once

#include <stddef.h>
#include <stdint.h>

#include <ktl/ref>

#include "kernel/mm/page.h"
#include "kernel/mm/paging.h"
#include "kernel/mm/region.h"
#include "kernel/synchronization/spinlock.h"

namespace kernel::mm {

// The one VMM lock. Guards all mutable VMM state -- region trees, VMO
// residency, page descriptors, mapping back-refs -- including the fault
// handler path. Take it with kernel::synchronization::lock_guard (irq-safe).
// Rule: VMM code never touches faultable memory while holding this lock; all
// VMM structures live in HHDM-mapped PMM frames, which are always resident.
// ponytail: single global lock; split per-aspace/per-VMO when scheduler-era
// contention is measured.
extern kernel::synchronization::spinlock g_vmm_lock;

// Arch-neutral address space: the arch page tables plus the portable state
// that hangs off them. Not a kernel Object and has no handle -- aspaces are
// infrastructure, not capabilities.
class vm_aspace {
   public:
    vm_aspace()                            = default;

    vm_aspace(const vm_aspace&)            = delete;
    vm_aspace& operator=(const vm_aspace&) = delete;

    // Fresh space with the kernel half cloned in and a root region spanning
    // the low canonical half (minus the null page).
    bool init();

    address_space& arch() { return m_arch; }
    const address_space& arch() const { return m_arch; }

    // Root of this space's region tree; valid after init()/vmm_init().
    Region& root() { return *m_root; }
    bool has_root() const { return m_root.get() != nullptr; }

    // Fault accounting, bumped by the fault handler.
    void count_fault() { ++m_faults; }
    uint64_t fault_count() const { return m_faults; }

    // Root region ref lands with the region-tree phase.

   private:
    friend void vmm_init(const vm_page_region*, size_t, const vm_page_region*, size_t);

    address_space m_arch;
    ktl::ref<Region> m_root;
    uint64_t m_faults = 0;
};

// The global kernel address space, valid after vmm_init(). It wraps the live
// boot page tables and is never destroyed.
vm_aspace& kernel_aspace();

// The global wired zero page, allocated at vmm_init(). Unpopulated anonymous
// pages map it read-only; the first write copies (CoW).
vm_paddr_t vmm_zero_page();

// Arch-neutral description of a memory access fault, decoded from the arch
// trap frame (CR2 + error code on x86_64).
struct vm_fault {
    uintptr_t vaddr;
    bool write;    // access was a write
    bool present;  // a translation existed (permission fault, CoW candidate)
    bool user;     // access came from user mode
};

// Demand-paging resolution: region lookup, authorization, pager fill, CoW.
// Returns true when the fault was resolved and the access should retry;
// false falls through to the crash path unchanged.
bool vmm_handle_fault(const vm_fault& fault);

// VMM initialization: sizes and fills the page descriptor array from the boot
// memory map (usable ranges become FREE, kernel/wired ranges stay WIRED) and
// adopts the live boot CR3 as the kernel aspace. Panics on failure -- the
// kernel cannot run without its descriptor array.
void vmm_init(const vm_page_region* usable, size_t usable_count, const vm_page_region* wired, size_t wired_count);

}  // namespace kernel::mm
