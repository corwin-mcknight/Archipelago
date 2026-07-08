#include "kernel/mm/arch_paging.h"

// riscv64 (Sv48) PTE codec and MMU primitives behind the shared page-walk
// machinery in mm/paging.cpp.
namespace kernel::mm::arch {

namespace {

// Sv48 page-table entry bits. Private to this file: portable code speaks
// arch-neutral prot/cache and never reasons about these bit positions. An
// entry with any of R/W/X set is a leaf at that level; a valid entry with
// none is a pointer to the next-level table.
namespace pte {
constexpr uint64_t VALID     = 1ull << 0;
constexpr uint64_t READ      = 1ull << 1;
constexpr uint64_t WRITE     = 1ull << 2;
constexpr uint64_t EXECUTE   = 1ull << 3;
constexpr uint64_t USER      = 1ull << 4;
constexpr uint64_t ACCESSED  = 1ull << 6;
constexpr uint64_t DIRTY     = 1ull << 7;
constexpr uint64_t RWX       = READ | WRITE | EXECUTE;

// Sv48 reserves bits 9:8 for software (RSW). The installed vm_cache_mode is
// stashed there so walk_ext can report it faithfully -- without Svpbmt the
// hardware has no cache-attribute bits at all, and PMAs decide the actual
// behavior. Bootloader-installed leaves carry RSW=0, which decodes to CACHED.
constexpr uint64_t RSW_SHIFT = 8;
constexpr uint64_t RSW_MASK  = 0x3ull << RSW_SHIFT;

// Physical page number sits at bits 53:10.
constexpr uint64_t PPN_MASK  = ((1ull << 44) - 1) << 10;
constexpr uint64_t ppn_encode(uint64_t paddr) { return ((paddr >> 12) << 10) & PPN_MASK; }
constexpr uint64_t ppn_decode(uint64_t entry) { return ((entry & PPN_MASK) >> 10) << 12; }
}  // namespace pte

// satp: mode 9 (Sv48) in bits 63:60, root table PPN in bits 43:0.
constexpr uint64_t SATP_MODE_SV48 = 9ull << 60;
constexpr uint64_t SATP_PPN_MASK  = (1ull << 44) - 1;

}  // namespace

bool pte_present(uint64_t entry) { return (entry & pte::VALID) != 0; }
bool pte_leaf(uint64_t entry) { return (entry & pte::RWX) != 0; }
// A pointer-shaped entry (RWX=0) at the deepest level is malformed.
bool pte_leaf_bottom(uint64_t entry) { return (entry & pte::RWX) != 0; }
vm_paddr_t pte_addr(uint64_t entry) { return pte::ppn_decode(entry); }
uint64_t pte_set_addr(uint64_t entry, vm_paddr_t paddr) { return (entry & ~pte::PPN_MASK) | pte::ppn_encode(paddr); }

// Sv48 intermediate entries are pure pointers: permissions live at the leaf,
// so the installed mapping's flags are irrelevant here.
uint64_t make_table_ptr(vm_paddr_t child, uint64_t) { return pte::ppn_encode(child) | pte::VALID; }
void widen_table_ptr(uint64_t&, uint64_t) {}

uint64_t make_leaf(vm_paddr_t paddr, uint64_t flags) { return pte::ppn_encode(paddr) | flags | pte::VALID; }

// Translate arch-neutral leaf attributes into Sv48 PTE permission bits.
// A/D are pre-set so implementations that trap on hardware A/D update
// (Svade behavior) never fault on first touch. Sv48 has no cache-mode bits
// without the Svpbmt extension, so DEVICE/WRITE_COMBINING degrade to the
// platform default; QEMU virt's MMIO regions are unaffected by PMAs here.
uint64_t leaf_flags(vm_prot_t prot, vm_cache_mode cache) {
    uint64_t bits = pte::ACCESSED | pte::DIRTY;
    if (prot & vm_prot::READ) { bits |= pte::READ; }
    if (prot & vm_prot::WRITE) { bits |= pte::WRITE; }
    if (prot & vm_prot::EXECUTE) { bits |= pte::EXECUTE; }
    if (prot & vm_prot::USER) { bits |= pte::USER; }
    bits |= (static_cast<uint64_t>(cache) << pte::RSW_SHIFT) & pte::RSW_MASK;
    return bits;
}

// Recover arch-neutral attributes from a terminal (leaf) PTE.
vm_translation attrs_from_pte(uint64_t entry, vm_paddr_t paddr) {
    vm_prot_t prot = 0;
    if (entry & pte::READ) { prot |= vm_prot::READ; }
    if (entry & pte::WRITE) { prot |= vm_prot::WRITE; }
    if (entry & pte::EXECUTE) { prot |= vm_prot::EXECUTE; }
    if (entry & pte::USER) { prot |= vm_prot::USER; }
    auto cache = static_cast<vm_cache_mode>((entry & pte::RSW_MASK) >> pte::RSW_SHIFT);
    return {paddr, prot, cache};
}

vm_paddr_t current_root() {
    uint64_t satp;
    asm volatile("csrr %0, satp" : "=r"(satp));
    return (satp & SATP_PPN_MASK) << 12;
}

void set_root(vm_paddr_t root) {
    uint64_t satp = SATP_MODE_SV48 | ((root >> 12) & SATP_PPN_MASK);
    asm volatile("csrw satp, %0\n\tsfence.vma zero, zero" ::"r"(satp) : "memory");
}

// Invalidate the TLB entry for vaddr if this address space is live on this
// CPU. Cross-CPU TLB shootdown is future work; only one CPU is active today.
void flush_tlb_page(vm_paddr_t root, uintptr_t vaddr) {
    if (current_root() != root) { return; }
    asm volatile("sfence.vma %0, zero" ::"r"(vaddr) : "memory");
}

// Pre-Svvptc implementations may cache the invalid translation from the
// demand fault that installed this leaf; without a flush the retry can
// re-fault on the now-present page. QEMU doesn't cache invalid entries, so
// only real hardware sees this.
void flush_new_leaf(vm_paddr_t root, uintptr_t vaddr) { flush_tlb_page(root, vaddr); }

}  // namespace kernel::mm::arch
