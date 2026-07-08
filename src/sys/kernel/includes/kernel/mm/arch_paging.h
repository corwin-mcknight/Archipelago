#pragma once

#include <kernel/mm/vm_aspace.h>
#include <stdint.h>

// Per-arch PTE codec and MMU primitives behind the shared 4-level walk in
// mm/paging.cpp. Both supported architectures use 512-entry tables, four
// levels, and bit-47 canonical addresses; only the entry encoding and the
// TLB/root instructions differ. Implemented in <arch>/paging.cpp.
namespace kernel::mm::arch {

bool pte_present(uint64_t entry);
// Terminal data mapping by encoding alone (huge/RWX); the deepest level is
// judged by pte_leaf_bottom instead.
bool pte_leaf(uint64_t entry);
// True if a present entry at the deepest level is a data mapping. x86_64 has
// no other shape; riscv64 rejects pointer-shaped (RWX=0) entries as malformed.
bool pte_leaf_bottom(uint64_t entry);
vm_paddr_t pte_addr(uint64_t entry);
uint64_t pte_set_addr(uint64_t entry, vm_paddr_t paddr);
// Build an intermediate table pointer. The leaf flags of the mapping being
// installed are passed through so x86_64 can propagate the USER bit, which it
// requires at every level of the walk.
uint64_t make_table_ptr(vm_paddr_t child, uint64_t leaf_flags);
// Widen an existing intermediate for the same reason; no-op on riscv64.
void widen_table_ptr(uint64_t& slot, uint64_t leaf_flags);
uint64_t make_leaf(vm_paddr_t paddr, uint64_t flags);
uint64_t leaf_flags(vm_prot_t prot, vm_cache_mode cache);
vm_translation attrs_from_pte(uint64_t entry, vm_paddr_t paddr);

vm_paddr_t current_root();
void set_root(vm_paddr_t root);
// Invalidate one page's translation if root is live on this CPU.
void flush_tlb_page(vm_paddr_t root, uintptr_t vaddr);
// Post-install flush for a brand-new leaf: pre-Svvptc riscv64 may cache the
// failed translation that faulted us here; a no-op on x86_64.
void flush_new_leaf(vm_paddr_t root, uintptr_t vaddr);

}  // namespace kernel::mm::arch
