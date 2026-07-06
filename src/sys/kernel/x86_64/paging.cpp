#include "kernel/mm/pmm.h"
#include "kernel/mm/vm_aspace.h"

extern uintptr_t g_hhdm_offset;

namespace kernel::mm {

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

// The one currently active address space on this CPU. A plain global until
// per-CPU storage exists; single-CPU scoped. The fault handler resolves
// against it.
vm_aspace* g_active_space = nullptr;

inline uint64_t* table_at(vm_paddr_t paddr) { return reinterpret_cast<uint64_t*>(paddr + g_hhdm_offset); }

constexpr size_t pml4_index(uintptr_t vaddr) { return (vaddr >> 39) & 0x1FF; }
constexpr size_t pdpt_index(uintptr_t vaddr) { return (vaddr >> 30) & 0x1FF; }
constexpr size_t pd_index(uintptr_t vaddr) { return (vaddr >> 21) & 0x1FF; }
constexpr size_t pt_index(uintptr_t vaddr) { return (vaddr >> 12) & 0x1FF; }

// The index helpers only consume bits 12..47. An address is canonical iff
// sign-extending bit 47 reproduces bits 48..63, so bits 48..63 carry no extra
// information; reject non-canonical addresses rather than silently aliasing.
// This bit-47 reasoning is arch-specific and never leaks past this file.
constexpr bool is_canonical(uintptr_t vaddr) {
    int64_t s = static_cast<int64_t>(vaddr) >> 47;
    return s == 0 || s == -1;
}

ktl::maybe<vm_paddr_t> alloc_table() { return g_page_frame_allocator.alloc(); }

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

inline vm_paddr_t current_cr3() {
    uintptr_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3 & pte::ADDR_MASK;
}

// Invalidate the TLB entry for vaddr if this address space is live on this
// CPU. Cross-CPU TLB shootdown is future work; only one CPU is active today.
void flush_tlb_page(vm_paddr_t pml4_phys, uintptr_t vaddr) {
    if (current_cr3() != (pml4_phys & pte::ADDR_MASK)) { return; }
    asm volatile("invlpg (%0)" ::"r"(vaddr) : "memory");
}

// Recursively free intermediate page-table pages owned by an entry. Levels go
// 4 (PML4) down to 1 (PT). PT-level entries point at user data pages and are
// not freed here -- ownership of leaf pages stays with the caller.
void free_subtree(uint64_t entry, int level) {
    if (!(entry & pte::PRESENT)) { return; }
    if (entry & pte::HUGE) { return; }
    if (level <= 1) { return; }

    vm_paddr_t child = entry & pte::ADDR_MASK;
    uint64_t* table  = table_at(child);
    for (size_t i = 0; i < 512; ++i) { free_subtree(table[i], level - 1); }
    g_page_frame_allocator.free(child);
}

// Walk down the tree, allocating intermediate tables as needed. Returns a
// pointer to the leaf-PT slot for vaddr, or nullptr on allocation failure or
// on collision with an existing huge mapping.
uint64_t* ensure_pt_slot(vm_paddr_t pml4_phys, uintptr_t vaddr, uint64_t intermediate_flags) {
    uint64_t* table   = table_at(pml4_phys);
    size_t indices[4] = {pml4_index(vaddr), pdpt_index(vaddr), pd_index(vaddr), pt_index(vaddr)};

    // Slots filled in by this walk, so a deeper allocation failure can unwind
    // them and leave no leaf-less subtree behind.
    uint64_t* new_slots[3];
    int new_count = 0;

    for (int level = 0; level < 3; ++level) {
        uint64_t& slot = table[indices[level]];
        if (!(slot & pte::PRESENT)) {
            auto child = alloc_table();
            if (!child.has_value()) {
                for (int i = new_count - 1; i >= 0; --i) {
                    g_page_frame_allocator.free(*new_slots[i] & pte::ADDR_MASK);
                    *new_slots[i] = 0;
                }
                return nullptr;
            }
            slot                   = (child.value() & pte::ADDR_MASK) | pte::PRESENT | intermediate_flags;
            new_slots[new_count++] = &slot;
        } else if (slot & pte::HUGE) {
            return nullptr;
        } else {
            // x86_64 requires USER at every level of the walk; widen an
            // existing kernel-only intermediate when a user mapping joins it.
            slot |= intermediate_flags & pte::USER;
        }
        table = table_at(slot & pte::ADDR_MASK);
    }
    return &table[indices[3]];
}

// Walk down the tree without allocating. Returns a pointer to the leaf-PT
// slot, or nullptr if any intermediate is missing or huge.
uint64_t* find_pt_slot(vm_paddr_t pml4_phys, uintptr_t vaddr) {
    uint64_t* table   = table_at(pml4_phys);
    size_t indices[4] = {pml4_index(vaddr), pdpt_index(vaddr), pd_index(vaddr), pt_index(vaddr)};

    for (int level = 0; level < 3; ++level) {
        uint64_t entry = table[indices[level]];
        if (!(entry & pte::PRESENT)) { return nullptr; }
        if (entry & pte::HUGE) { return nullptr; }
        table = table_at(entry & pte::ADDR_MASK);
    }
    return &table[indices[3]];
}

// Resolve vaddr to its terminal PTE, transparently handling 1G/2M huge pages
// installed by the bootloader in the kernel half. Returns the entry and the
// intra-page offset mask, or nothing if unmapped.
struct terminal_pte {
    uint64_t entry;
    uintptr_t offset_mask;
};
ktl::maybe<terminal_pte> resolve(vm_paddr_t pml4_phys, uintptr_t vaddr) {
    uint64_t* table    = table_at(pml4_phys);
    size_t indices[4]  = {pml4_index(vaddr), pdpt_index(vaddr), pd_index(vaddr), pt_index(vaddr)};
    // Offset masks per level a huge/leaf mapping can terminate at: 1G at the
    // PDPT, 2M at the PD, 4K at the PT.
    uintptr_t masks[4] = {(1ull << 39) - 1, (1ull << 30) - 1, (1ull << 21) - 1, (1ull << 12) - 1};

    for (int level = 0; level < 4; ++level) {
        uint64_t entry = table[indices[level]];
        if (!(entry & pte::PRESENT)) { return ktl::nothing; }
        if (level == 3) { return terminal_pte{entry, masks[3]}; }
        if (entry & pte::HUGE) { return terminal_pte{entry, masks[level]}; }
        table = table_at(entry & pte::ADDR_MASK);
    }
    return ktl::nothing;
}

}  // namespace

namespace {
// Deep-copy a page-table subtree into fresh PMM frames. Tables are copied at
// every level; huge and 4K leaf entries are kept verbatim (they point at data
// pages, not tables, and carry no ownership). Runs once at boot to move the
// kernel half off the bootloader's tables; on OOM the boot is already lost,
// so partially copied frames are not unwound (the caller panics).
ktl::maybe<vm_paddr_t> deep_copy_table(vm_paddr_t table_phys, int level) {
    auto copy = alloc_table();
    if (!copy.has_value()) { return ktl::nothing; }
    uint64_t* src = table_at(table_phys);
    uint64_t* dst = table_at(copy.value());
    for (size_t i = 0; i < 512; ++i) {
        uint64_t entry = src[i];
        // A present, non-huge entry above the PT level points at a child
        // table; PT entries point at data pages and are kept verbatim.
        if ((entry & pte::PRESENT) && !(entry & pte::HUGE) && level > 1) {
            auto child = deep_copy_table(entry & pte::ADDR_MASK, level - 1);
            if (!child.has_value()) { return ktl::nothing; }
            entry = (entry & ~pte::ADDR_MASK) | child.value();
        }
        dst[i] = entry;
    }
    return copy;
}
}  // namespace

bool vm_aspace::is_valid() const { return m_arch.pml4_phys != 0; }

bool vm_aspace::arch_init() {
    if (m_arch.pml4_phys != 0) { return false; }
    auto pml4 = alloc_table();
    if (!pml4.has_value()) { return false; }
    m_arch.pml4_phys = pml4.value();

    // Clone the kernel half (upper 256 PML4 entries) from the active space so
    // the kernel is mapped in every address space. The lower 256 (user half)
    // stay empty. The intermediate tables are shared, not copied -- acceptable
    // because kernel mappings are not created through map_page this milestone.
    uint64_t* dst    = table_at(m_arch.pml4_phys);
    uint64_t* src    = table_at(current_cr3());
    for (size_t i = 256; i < 512; ++i) { dst[i] = src[i]; }
    return true;
}

void vm_aspace::arch_destroy() {
    if (m_arch.pml4_phys == 0) { return; }
    // Only free the user half. The kernel half was cloned from the boot tables
    // and its subtrees are shared with every other space -- freeing them would
    // corrupt the kernel mapping.
    uint64_t* pml4 = table_at(m_arch.pml4_phys);
    for (size_t i = 0; i < 256; ++i) { free_subtree(pml4[i], 4); }
    g_page_frame_allocator.free(m_arch.pml4_phys);
    m_arch.pml4_phys = 0;
    if (g_active_space == this) { g_active_space = nullptr; }
}

bool vm_aspace::map_page(uintptr_t vaddr, vm_paddr_t paddr, vm_prot_t prot, vm_cache_mode cache) {
    if (m_arch.pml4_phys == 0) { return false; }
    if (!is_canonical(vaddr)) { return false; }
    if ((vaddr & 0xFFF) != 0) { return false; }
    if ((paddr & 0xFFF) != 0) { return false; }

    uint64_t flags        = leaf_flags(prot, cache);
    uint64_t intermediate = pte::WRITABLE | (flags & pte::USER);

    uint64_t* leaf        = ensure_pt_slot(m_arch.pml4_phys, vaddr, intermediate);
    if (leaf == nullptr) { return false; }

    // Present leaves are rejected rather than replaced, so no TLB flush is
    // needed here -- map_page never changes an existing translation.
    if (*leaf & pte::PRESENT) { return false; }

    *leaf = (paddr & pte::ADDR_MASK) | flags | pte::PRESENT;
    return true;
}

ktl::maybe<vm_paddr_t> vm_aspace::walk(uintptr_t vaddr) const {
    if (m_arch.pml4_phys == 0) { return ktl::nothing; }
    if (!is_canonical(vaddr)) { return ktl::nothing; }
    auto term = resolve(m_arch.pml4_phys, vaddr);
    if (!term.has_value()) { return ktl::nothing; }
    vm_paddr_t base = term.value().entry & pte::ADDR_MASK & ~term.value().offset_mask;
    return base | (vaddr & term.value().offset_mask);
}

ktl::maybe<vm_translation> vm_aspace::walk_ext(uintptr_t vaddr) const {
    if (m_arch.pml4_phys == 0) { return ktl::nothing; }
    if (!is_canonical(vaddr)) { return ktl::nothing; }
    auto term = resolve(m_arch.pml4_phys, vaddr);
    if (!term.has_value()) { return ktl::nothing; }
    vm_paddr_t base  = term.value().entry & pte::ADDR_MASK & ~term.value().offset_mask;
    vm_paddr_t paddr = base | (vaddr & term.value().offset_mask);
    return attrs_from_pte(term.value().entry, paddr);
}

ktl::maybe<vm_paddr_t> vm_aspace::unmap_page(uintptr_t vaddr) {
    if (m_arch.pml4_phys == 0) { return ktl::nothing; }
    if (!is_canonical(vaddr)) { return ktl::nothing; }
    if ((vaddr & 0xFFF) != 0) { return ktl::nothing; }

    // 4K only: huge mappings are bootloader-owned kernel mappings and are never
    // torn down through this path.
    uint64_t* leaf = find_pt_slot(m_arch.pml4_phys, vaddr);
    if (leaf == nullptr) { return ktl::nothing; }
    if (!(*leaf & pte::PRESENT)) { return ktl::nothing; }

    vm_paddr_t paddr = *leaf & pte::ADDR_MASK;
    *leaf            = 0;
    flush_tlb_page(m_arch.pml4_phys, vaddr);
    return paddr;
}

void vm_aspace::activate() {
    asm volatile("mov %0, %%cr3" ::"r"(m_arch.pml4_phys) : "memory");
    g_active_space = this;
}

vm_aspace* vm_aspace::active() { return g_active_space; }

bool vm_aspace::arch_init_kernel() {
    if (m_arch.pml4_phys != 0) { return false; }
    // Deep-copy the kernel half out of the bootloader's tables: Limine's
    // tables live in bootloader-reclaimable memory that the PMM treats as
    // usable, so the kernel must own every table frame it runs on. The lower
    // 256 entries (user half) start empty. The caller activates the space.
    auto pml4 = alloc_table();
    if (!pml4.has_value()) { return false; }
    m_arch.pml4_phys = pml4.value();

    uint64_t* dst    = table_at(m_arch.pml4_phys);
    uint64_t* src    = table_at(current_cr3());
    for (size_t i = 256; i < 512; ++i) {
        uint64_t entry = src[i];
        if ((entry & pte::PRESENT) && !(entry & pte::HUGE)) {
            auto child = deep_copy_table(entry & pte::ADDR_MASK, 3);
            if (!child.has_value()) { return false; }
            entry = (entry & ~pte::ADDR_MASK) | child.value();
        }
        dst[i] = entry;
    }
    return true;
}

// End of the canonical low half: bit 47 sign-extension boundary.
uintptr_t vm_aspace::low_limit() { return 0x0000800000000000ull; }

}  // namespace kernel::mm
