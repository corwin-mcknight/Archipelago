#pragma once

#include <stddef.h>
#include <stdint.h>

#include <ktl/maybe>
#include <ktl/ref>

#include "kernel/mm/arch_aspace.h"
#include "kernel/mm/page.h"
#include "kernel/mm/paging.h"
#include "kernel/mm/region.h"
#include "kernel/synchronization/spinlock.h"

namespace kernel::mm {

// Serializes region trees, VMO residency, and mapping back-refs, including
// fault handling. Use critical_irq_lock_guard to disable interrupts and
// preemption while held. VMM metadata stays resident; never access a
// faultable mapping while holding this lock.
extern kernel::synchronization::spinlock g_vmm_lock;

// An address space combines region ownership in mm/vm_aspace.cpp with the
// shared page walks and root management in mm/paging.cpp. Architecture hooks
// provide PTE encoding and MMU operations. Only the paging code accesses
// m_arch. Address spaces are infrastructure, not handle-bearing Objects.
class vm_aspace {
   public:
    vm_aspace() = default;
    ~vm_aspace();

    vm_aspace(const vm_aspace&)            = delete;
    vm_aspace& operator=(const vm_aspace&) = delete;

    // ---- region ownership and lifecycle (mm/vm_aspace.cpp) ----

    // Fresh space: arch tables with the kernel half cloned in, plus a root
    // region spanning the low half (minus the null page).
    bool init();
    // Tear down regions (zapping their translations), then the arch tables.
    void destroy();

    // Root of this space's region tree; valid after init()/vmm_init().
    Region& root() { return *m_root; }
    bool has_root() const { return m_root.get() != nullptr; }

    // Fault accounting, bumped by the fault handler.
    void count_fault() { ++m_faults; }
    uint64_t fault_count() const { return m_faults; }

    // ---- page tables (mm/paging.cpp) ----

    bool is_valid() const;
    // Install a 4K translation in the low address half with the given
    // protection and cache mode. Kernel-half mutations require a separate
    // mapping path because their intermediate tables are shared.
    bool map_page(uintptr_t vaddr, vm_paddr_t paddr, vm_prot_t prot, vm_cache_mode cache = vm_cache_mode::CACHED);
    // Resolve an address in either canonical half, preserving its page offset.
    ktl::maybe<vm_paddr_t> walk(uintptr_t vaddr) const;
    // Resolve and report the mapping's protection and cache mode as well.
    ktl::maybe<vm_translation> walk_ext(uintptr_t vaddr) const;
    // Remove a 4K translation in the low half; return its frame without freeing it.
    ktl::maybe<vm_paddr_t> unmap_page(uintptr_t vaddr);

    // Load this space's page tables and record it as active on the calling CPU.
    void activate();
    // The space recorded on this CPU, or null if none is recorded. The fault
    // handler resolves against this space.
    static vm_aspace* active();

    // Exclusive end of the low (non-kernel) virtual address half. The value is
    // arch-specific (canonical-form on x86_64, Sv39 on riscv64); portable
    // code treats it as an opaque limit.
    static uintptr_t low_limit();

   private:
    friend void vmm_init(const vm_page_region*, size_t, const vm_page_region*, size_t);

    // Page-table lifecycle. arch_init shallow-clones the kernel half (tables
    // shared with the active space); arch_init_kernel deep-copies it into
    // owned frames -- used once at vmm_init so the kernel runs on its own
    // tables, not the bootloader's. The kernel aspace is never destroyed.
    bool arch_init();
    bool arch_init_kernel();
    void arch_destroy();

    arch_aspace m_arch;
    ktl::ref<Region> m_root;
    uint64_t m_faults = 0;
};

// The global kernel address space, valid after vmm_init(). It owns its page
// tables (deep-copied off the bootloader's at init) and is never destroyed.
vm_aspace& kernel_aspace();

// The global wired zero page, allocated at vmm_init(). Unpopulated anonymous
// pages map it read-only; the first write allocates a private zeroed frame (CoW).
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
// memory map (usable ranges become FREE, kernel/wired ranges WIRED, holes
// and firmware ranges MMIO),
// then builds the kernel's own page tables and switches onto them. Panics on
// failure -- the kernel cannot run without either.
void vmm_init(const vm_page_region* usable, size_t usable_count, const vm_page_region* wired, size_t wired_count);

}  // namespace kernel::mm
