#pragma once

#include <kernel/mm/vm_aspace.h>
#include <stdint.h>

// Per-arch PTE codec and MMU primitives behind the shared page walk in
// mm/paging.cpp. Both supported architectures use 512-entry tables and radix-9
// levels; only the walk depth, the entry encoding, and the TLB/root
// instructions differ. Implemented in <arch>/paging.cpp.
namespace kernel::mm::arch {

// Walk depth: 4 levels on x86_64 (bit-47 canonical), 3 on riscv64 (Sv39,
// bit-38 canonical -- the deepest mode the JH7110's U74 cores implement, and
// the one mode the kernel uses everywhere so QEMU exercises the same tables
// as real boards).
#ifdef ARCH_RISCV64
constexpr int PT_LEVELS = 3;
#else
constexpr int PT_LEVELS = 4;
#endif
// Virtual-address bits the walk consumes: 12-bit page offset plus 9 per level.
constexpr int VA_BITS = 12 + 9 * PT_LEVELS;

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
// Invalidate one page's translation on every core in core_mask (bit n = dense core n), none of
// which is the caller. The caller established that the space is live on each of them.
void shootdown_tlb_page(uint64_t core_mask, uintptr_t vaddr);
// Post-install flush for a brand-new leaf: pre-Svvptc riscv64 may cache the
// failed translation that faulted us here; a no-op on x86_64.
void flush_new_leaf(vm_paddr_t root, uintptr_t vaddr);

}  // namespace kernel::mm::arch
