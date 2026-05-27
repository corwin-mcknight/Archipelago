#include "kernel/mm/paging.h"

#include "kernel/mm/pmm.h"

extern uintptr_t g_hhdm_offset;

namespace kernel::mm {

namespace {

inline uint64_t* table_at(vm_paddr_t paddr) { return reinterpret_cast<uint64_t*>(paddr + g_hhdm_offset); }

constexpr size_t pml4_index(uintptr_t vaddr) { return (vaddr >> 39) & 0x1FF; }
constexpr size_t pdpt_index(uintptr_t vaddr) { return (vaddr >> 30) & 0x1FF; }
constexpr size_t pd_index(uintptr_t vaddr) { return (vaddr >> 21) & 0x1FF; }
constexpr size_t pt_index(uintptr_t vaddr) { return (vaddr >> 12) & 0x1FF; }

ktl::maybe<vm_paddr_t> alloc_table() { return g_page_frame_allocator.alloc(); }

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

    for (int level = 0; level < 3; ++level) {
        uint64_t& slot = table[indices[level]];
        if (!(slot & pte::PRESENT)) {
            auto child = alloc_table();
            if (!child.has_value()) { return nullptr; }
            slot = (child.value() & pte::ADDR_MASK) | pte::PRESENT | intermediate_flags;
        } else if (slot & pte::HUGE) {
            return nullptr;
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

}  // namespace

bool address_space::init() {
    if (m_pml4_phys != 0) { return false; }
    auto pml4 = alloc_table();
    if (!pml4.has_value()) { return false; }
    m_pml4_phys = pml4.value();
    return true;
}

void address_space::destroy() {
    if (m_pml4_phys == 0) { return; }
    uint64_t* pml4 = table_at(m_pml4_phys);
    for (size_t i = 0; i < 512; ++i) { free_subtree(pml4[i], 4); }
    g_page_frame_allocator.free(m_pml4_phys);
    m_pml4_phys = 0;
}

bool address_space::map_page(uintptr_t vaddr, vm_paddr_t paddr, uint64_t flags) {
    if (m_pml4_phys == 0) { return false; }
    if ((vaddr & 0xFFF) != 0) { return false; }
    if ((paddr & 0xFFF) != 0) { return false; }

    uint64_t intermediate = pte::WRITABLE | (flags & pte::USER);

    uint64_t* leaf        = ensure_pt_slot(m_pml4_phys, vaddr, intermediate);
    if (leaf == nullptr) { return false; }
    if (*leaf & pte::PRESENT) { return false; }

    *leaf = (paddr & pte::ADDR_MASK) | (flags | pte::PRESENT);
    return true;
}

ktl::maybe<vm_paddr_t> address_space::walk(uintptr_t vaddr) const {
    if (m_pml4_phys == 0) { return ktl::nothing; }
    uint64_t* leaf = find_pt_slot(m_pml4_phys, vaddr);
    if (leaf == nullptr) { return ktl::nothing; }
    if (!(*leaf & pte::PRESENT)) { return ktl::nothing; }
    return (*leaf & pte::ADDR_MASK) | (vaddr & 0xFFF);
}

ktl::maybe<vm_paddr_t> address_space::unmap_page(uintptr_t vaddr) {
    if (m_pml4_phys == 0) { return ktl::nothing; }
    if ((vaddr & 0xFFF) != 0) { return ktl::nothing; }

    uint64_t* leaf = find_pt_slot(m_pml4_phys, vaddr);
    if (leaf == nullptr) { return ktl::nothing; }
    if (!(*leaf & pte::PRESENT)) { return ktl::nothing; }

    vm_paddr_t paddr = *leaf & pte::ADDR_MASK;
    *leaf            = 0;
    return paddr;
}

}  // namespace kernel::mm
