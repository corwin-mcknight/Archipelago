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

// The one VMM lock. Guards all mutable VMM state -- region trees, VMO
// residency, page descriptors, mapping back-refs -- including the fault
// handler path. Take it with kernel::synchronization::lock_guard (irq-safe).
// Rule: VMM code never touches faultable memory while holding this lock; all
// VMM structures live in HHDM-mapped PMM frames, which are always resident.
// ponytail: single global lock; split per-aspace/per-VMO when scheduler-era
// contention is measured.
extern kernel::synchronization::spinlock g_vmm_lock;

// An address space: one class, completed by each architecture. The portable
// half (region tree, fault accounting, lifecycle) is implemented in
// mm/vm_aspace.cpp; the paging half (map/walk/unmap/activate) is implemented
// by the architecture (x86_64/paging.cpp), which also supplies the shape of
// the embedded arch_aspace state. Portable code never touches that state.
// Not a kernel Object and has no handle -- aspaces are infrastructure, not
// capabilities.
class vm_aspace {
   public:
    vm_aspace() = default;
    ~vm_aspace();

    vm_aspace(const vm_aspace&)            = delete;
    vm_aspace& operator=(const vm_aspace&) = delete;

    // ---- portable half (mm/vm_aspace.cpp) ----

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

    // ---- arch-completed half (x86_64/paging.cpp) ----

    bool is_valid() const;
    // Install a 4K translation with the given protection and cache mode.
    bool map_page(uintptr_t vaddr, vm_paddr_t paddr, vm_prot_t prot, vm_cache_mode cache = vm_cache_mode::CACHED);
    // Resolve a virtual address to its physical address (offset preserved).
    ktl::maybe<vm_paddr_t> walk(uintptr_t vaddr) const;
    // Resolve and report the mapping's protection and cache mode as well.
    ktl::maybe<vm_translation> walk_ext(uintptr_t vaddr) const;
    ktl::maybe<vm_paddr_t> unmap_page(uintptr_t vaddr);

    // Load this space's page tables into the CPU and record it as the active
    // space. Single-CPU scoped; cross-CPU shootdown is out of scope.
    void activate();

    // Exclusive end of the low (non-kernel) virtual address half. The value is
    // arch-specific (canonical-form on x86_64, Sv39/Sv48 on riscv64); portable
    // code treats it as an opaque limit.
    static uintptr_t low_limit();

   private:
    friend void vmm_init(const vm_page_region*, size_t, const vm_page_region*, size_t);

    // Arch internals. arch_init shallow-clones the kernel half (tables
    // shared with the active space); arch_init_kernel deep-copies it into
    // owned frames -- used once at vmm_init so the kernel runs on its own
    // tables, not the bootloader's. The kernel aspace is never destroyed.
    bool arch_init();
    bool arch_init_kernel();
    void arch_destroy();

    arch_aspace m_arch;  // shape differs per architecture
    ktl::ref<Region> m_root;
    uint64_t m_faults = 0;
};

// The global kernel address space, valid after vmm_init(). It owns its page
// tables (deep-copied off the bootloader's at init) and is never destroyed.
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
// memory map (usable ranges become FREE, kernel/wired ranges stay WIRED),
// then builds the kernel's own page tables and switches onto them. Panics on
// failure -- the kernel cannot run without either.
void vmm_init(const vm_page_region* usable, size_t usable_count, const vm_page_region* wired, size_t wired_count);

}  // namespace kernel::mm
