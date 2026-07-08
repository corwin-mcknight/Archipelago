#include "kernel/mm/arch_paging.h"

// x86_64 PTE codec and MMU primitives behind the shared page-walk machinery
// in mm/paging.cpp.
namespace kernel::mm::arch {

namespace {

// x86_64 page-table entry bits. Private to this file: portable code speaks
// arch-neutral prot/cache and never reasons about these bit positions.
namespace pte {
constexpr uint64_t PRESENT       = 1ull << 0;
constexpr uint64_t WRITABLE      = 1ull << 1;
constexpr uint64_t USER          = 1ull << 2;
constexpr uint64_t CACHE_DISABLE = 1ull << 4;
constexpr uint64_t HUGE          = 1ull << 7;
constexpr uint64_t NO_EXECUTE    = 1ull << 63;

constexpr uint64_t ADDR_MASK     = 0x000FFFFFFFFFF000ull;
}  // namespace pte

}  // namespace

bool pte_present(uint64_t entry) { return (entry & pte::PRESENT) != 0; }
bool pte_leaf(uint64_t entry) { return (entry & pte::HUGE) != 0; }
// x86_64 PT entries have no other shape: present at the bottom means data.
bool pte_leaf_bottom(uint64_t) { return true; }
vm_paddr_t pte_addr(uint64_t entry) { return entry & pte::ADDR_MASK; }
uint64_t pte_set_addr(uint64_t entry, vm_paddr_t paddr) { return (entry & ~pte::ADDR_MASK) | paddr; }

// x86_64 requires the permission bits at every level of the walk;
// intermediates get WRITABLE plus the USER bit of the mapping being installed.
uint64_t make_table_ptr(vm_paddr_t child, uint64_t leaf_flags) {
    return (child & pte::ADDR_MASK) | pte::PRESENT | pte::WRITABLE | (leaf_flags & pte::USER);
}
// Widen an existing kernel-only intermediate when a user mapping joins it.
void widen_table_ptr(uint64_t& slot, uint64_t leaf_flags) { slot |= leaf_flags & pte::USER; }

uint64_t make_leaf(vm_paddr_t paddr, uint64_t flags) { return (paddr & pte::ADDR_MASK) | flags | pte::PRESENT; }

// Translate arch-neutral leaf attributes into x86_64 PTE permission/cache bits.
uint64_t leaf_flags(vm_prot_t prot, vm_cache_mode cache) {
    uint64_t bits = 0;
    if (prot & vm_prot::WRITE) { bits |= pte::WRITABLE; }
    if (prot & vm_prot::USER) { bits |= pte::USER; }
    // x86_64 has no read-enable bit -- a present page is always readable.
    // Absence of EXECUTE becomes the NX bit (EFER.NXE is enabled at boot).
    if (!(prot & vm_prot::EXECUTE)) { bits |= pte::NO_EXECUTE; }
    // DEVICE and WRITE_COMBINING both degrade to uncached (PCD) until PAT
    // programming exists; CACHED leaves the write-back default.
    if (cache == vm_cache_mode::DEVICE || cache == vm_cache_mode::WRITE_COMBINING) { bits |= pte::CACHE_DISABLE; }
    return bits;
}

// Recover arch-neutral attributes from a terminal (leaf or huge) PTE.
vm_translation attrs_from_pte(uint64_t entry, vm_paddr_t paddr) {
    vm_prot_t prot = vm_prot::READ;
    if (entry & pte::WRITABLE) { prot |= vm_prot::WRITE; }
    if (entry & pte::USER) { prot |= vm_prot::USER; }
    if (!(entry & pte::NO_EXECUTE)) { prot |= vm_prot::EXECUTE; }
    vm_cache_mode cache = (entry & pte::CACHE_DISABLE) ? vm_cache_mode::DEVICE : vm_cache_mode::CACHED;
    return {paddr, prot, cache};
}

vm_paddr_t current_root() {
    uintptr_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3 & pte::ADDR_MASK;
}

void set_root(vm_paddr_t root) { asm volatile("mov %0, %%cr3" ::"r"(root) : "memory"); }

// Invalidate the TLB entry for vaddr if this address space is live on this
// CPU. Cross-CPU TLB shootdown is future work; only one CPU is active today.
void flush_tlb_page(vm_paddr_t root, uintptr_t vaddr) {
    if (current_root() != (root & pte::ADDR_MASK)) { return; }
    asm volatile("invlpg (%0)" ::"r"(vaddr) : "memory");
}

// x86_64 never caches failed translations, so a freshly installed leaf needs
// no flush.
void flush_new_leaf(vm_paddr_t, uintptr_t) {}

}  // namespace kernel::mm::arch
