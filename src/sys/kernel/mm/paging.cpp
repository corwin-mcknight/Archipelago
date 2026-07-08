#include "kernel/mm/arch_paging.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/vm_aspace.h"

extern uintptr_t g_hhdm_offset;

// Arch-neutral 4-level page-walk machinery and the vm_aspace paging methods.
// Everything that depends on the PTE encoding or MMU instructions goes
// through the kernel::mm::arch hooks (see arch_paging.h).
namespace kernel::mm {

namespace {

// The one currently active address space on this CPU. A plain global until
// per-CPU storage exists; single-CPU scoped. The fault handler resolves
// against it.
vm_aspace* g_active_space = nullptr;

inline uint64_t* table_at(vm_paddr_t paddr) { return reinterpret_cast<uint64_t*>(paddr + g_hhdm_offset); }

constexpr size_t level_index(uintptr_t vaddr, int level) { return (vaddr >> (39 - 9 * level)) & 0x1FF; }

// The index helpers only consume bits 12..47. An address is canonical iff
// sign-extending bit 47 reproduces bits 48..63 (the same shape on both
// supported arches); reject non-canonical addresses rather than silently
// aliasing.
constexpr bool is_canonical(uintptr_t vaddr) {
    int64_t s = static_cast<int64_t>(vaddr) >> 47;
    return s == 0 || s == -1;
}

ktl::maybe<vm_paddr_t> alloc_table() { return g_page_frame_allocator.alloc(); }

// Recursively free intermediate page-table pages owned by an entry. Levels go
// 4 (root) down to 1 (leaf table). Leaf-table entries point at user data
// pages and are not freed here -- ownership of leaf pages stays with the
// caller.
void free_subtree(uint64_t entry, int level) {
    if (!arch::pte_present(entry)) { return; }
    if (arch::pte_leaf(entry)) { return; }  // large-page data mapping, not a table
    if (level <= 1) { return; }

    vm_paddr_t child = arch::pte_addr(entry);
    uint64_t* table  = table_at(child);
    for (size_t i = 0; i < 512; ++i) { free_subtree(table[i], level - 1); }
    g_page_frame_allocator.free(child);
}

// Walk down the tree, allocating intermediate tables as needed. Returns a
// pointer to the leaf-table slot for vaddr, or nullptr on allocation failure
// or on collision with an existing large-page mapping.
uint64_t* ensure_leaf_slot(vm_paddr_t root_phys, uintptr_t vaddr, uint64_t leaf_flags) {
    uint64_t* table = table_at(root_phys);

    // Slots filled in by this walk, so a deeper allocation failure can unwind
    // them and leave no leaf-less subtree behind.
    uint64_t* new_slots[3];
    int new_count = 0;

    for (int level = 0; level < 3; ++level) {
        uint64_t& slot = table[level_index(vaddr, level)];
        if (!arch::pte_present(slot)) {
            auto child = alloc_table();
            if (!child.has_value()) {
                for (int i = new_count - 1; i >= 0; --i) {
                    g_page_frame_allocator.free(arch::pte_addr(*new_slots[i]));
                    *new_slots[i] = 0;
                }
                return nullptr;
            }
            slot                   = arch::make_table_ptr(child.value(), leaf_flags);
            new_slots[new_count++] = &slot;
        } else if (arch::pte_leaf(slot)) {
            return nullptr;
        } else {
            arch::widen_table_ptr(slot, leaf_flags);
        }
        table = table_at(arch::pte_addr(slot));
    }
    return &table[level_index(vaddr, 3)];
}

// Walk down the tree without allocating. Returns a pointer to the leaf-table
// slot, or nullptr if any intermediate is missing or a large-page leaf.
uint64_t* find_leaf_slot(vm_paddr_t root_phys, uintptr_t vaddr) {
    uint64_t* table = table_at(root_phys);

    for (int level = 0; level < 3; ++level) {
        uint64_t entry = table[level_index(vaddr, level)];
        if (!arch::pte_present(entry)) { return nullptr; }
        if (arch::pte_leaf(entry)) { return nullptr; }
        table = table_at(arch::pte_addr(entry));
    }
    return &table[level_index(vaddr, 3)];
}

// Resolve vaddr to its terminal PTE, transparently handling 512G/1G/2M large
// pages installed by the bootloader in the kernel half. Returns the entry and
// the intra-page offset mask, or nothing if unmapped.
struct terminal_pte {
    uint64_t entry;
    uintptr_t offset_mask;
};
ktl::maybe<terminal_pte> resolve(vm_paddr_t root_phys, uintptr_t vaddr) {
    uint64_t* table    = table_at(root_phys);
    // Offset masks per level a large/leaf mapping can terminate at.
    uintptr_t masks[4] = {(1ull << 39) - 1, (1ull << 30) - 1, (1ull << 21) - 1, (1ull << 12) - 1};

    for (int level = 0; level < 4; ++level) {
        uint64_t entry = table[level_index(vaddr, level)];
        if (!arch::pte_present(entry)) { return ktl::nothing; }
        if (level == 3) {
            if (!arch::pte_leaf_bottom(entry)) { return ktl::nothing; }  // malformed pointer at leaf level
            return terminal_pte{entry, masks[3]};
        }
        if (arch::pte_leaf(entry)) { return terminal_pte{entry, masks[level]}; }
        table = table_at(arch::pte_addr(entry));
    }
    return ktl::nothing;
}

// Deep-copy a page-table subtree into fresh PMM frames. Tables are copied at
// every level; leaf entries (any size) are kept verbatim (they point at data
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
        // A present pointer entry above the leaf level references a child
        // table; leaf entries are kept verbatim.
        if (arch::pte_present(entry) && !arch::pte_leaf(entry) && level > 1) {
            auto child = deep_copy_table(arch::pte_addr(entry), level - 1);
            if (!child.has_value()) { return ktl::nothing; }
            entry = arch::pte_set_addr(entry, child.value());
        }
        dst[i] = entry;
    }
    return copy;
}

}  // namespace

bool vm_aspace::is_valid() const { return m_arch.root_phys != 0; }

bool vm_aspace::arch_init() {
    if (m_arch.root_phys != 0) { return false; }
    auto root = alloc_table();
    if (!root.has_value()) { return false; }
    m_arch.root_phys = root.value();

    // Clone the kernel half (upper 256 root entries) from the active space so
    // the kernel is mapped in every address space. The lower 256 (user half)
    // stay empty. The intermediate tables are shared, not copied -- acceptable
    // because kernel mappings are not created through map_page this milestone.
    uint64_t* dst    = table_at(m_arch.root_phys);
    uint64_t* src    = table_at(arch::current_root());
    for (size_t i = 256; i < 512; ++i) { dst[i] = src[i]; }
    return true;
}

bool vm_aspace::arch_init_kernel() {
    if (m_arch.root_phys != 0) { return false; }
    // Deep-copy the kernel half out of the bootloader's tables: Limine's
    // tables live in bootloader-reclaimable memory the kernel does not own,
    // so the kernel must own every table frame it runs on. The lower 256
    // entries (user half) start empty. The caller activates the space.
    auto root = alloc_table();
    if (!root.has_value()) { return false; }
    m_arch.root_phys = root.value();

    uint64_t* dst    = table_at(m_arch.root_phys);
    uint64_t* src    = table_at(arch::current_root());
    for (size_t i = 256; i < 512; ++i) {
        uint64_t entry = src[i];
        if (arch::pte_present(entry) && !arch::pte_leaf(entry)) {
            auto child = deep_copy_table(arch::pte_addr(entry), 3);
            if (!child.has_value()) { return false; }
            entry = arch::pte_set_addr(entry, child.value());
        }
        dst[i] = entry;
    }
    return true;
}

void vm_aspace::arch_destroy() {
    if (m_arch.root_phys == 0) { return; }
    // Only free the user half. The kernel half was cloned from the boot tables
    // and its subtrees are shared with every other space -- freeing them would
    // corrupt the kernel mapping.
    uint64_t* root = table_at(m_arch.root_phys);
    for (size_t i = 0; i < 256; ++i) { free_subtree(root[i], 4); }
    g_page_frame_allocator.free(m_arch.root_phys);
    m_arch.root_phys = 0;
    if (g_active_space == this) { g_active_space = nullptr; }
}

bool vm_aspace::map_page(uintptr_t vaddr, vm_paddr_t paddr, vm_prot_t prot, vm_cache_mode cache) {
    if (m_arch.root_phys == 0) { return false; }
    if (!is_canonical(vaddr)) { return false; }
    if ((vaddr & 0xFFF) != 0) { return false; }
    if ((paddr & 0xFFF) != 0) { return false; }
    // Uniform contract: every present page is readable. x86_64 cannot encode
    // anything else; on riscv64 a READ-less prot would emit a reserved or
    // pointer-shaped PTE.
    if (!(prot & vm_prot::READ)) { return false; }

    uint64_t flags = arch::leaf_flags(prot, cache);
    uint64_t* leaf = ensure_leaf_slot(m_arch.root_phys, vaddr, flags);
    if (leaf == nullptr) { return false; }

    // Present leaves are rejected rather than replaced, so map_page never
    // changes an existing translation.
    if (arch::pte_present(*leaf)) { return false; }

    *leaf = arch::make_leaf(paddr, flags);
    arch::flush_new_leaf(m_arch.root_phys, vaddr);
    return true;
}

ktl::maybe<vm_paddr_t> vm_aspace::walk(uintptr_t vaddr) const {
    if (m_arch.root_phys == 0) { return ktl::nothing; }
    if (!is_canonical(vaddr)) { return ktl::nothing; }
    auto term = resolve(m_arch.root_phys, vaddr);
    if (!term.has_value()) { return ktl::nothing; }
    vm_paddr_t base = arch::pte_addr(term.value().entry) & ~term.value().offset_mask;
    return base | (vaddr & term.value().offset_mask);
}

ktl::maybe<vm_translation> vm_aspace::walk_ext(uintptr_t vaddr) const {
    if (m_arch.root_phys == 0) { return ktl::nothing; }
    if (!is_canonical(vaddr)) { return ktl::nothing; }
    auto term = resolve(m_arch.root_phys, vaddr);
    if (!term.has_value()) { return ktl::nothing; }
    vm_paddr_t base  = arch::pte_addr(term.value().entry) & ~term.value().offset_mask;
    vm_paddr_t paddr = base | (vaddr & term.value().offset_mask);
    return arch::attrs_from_pte(term.value().entry, paddr);
}

ktl::maybe<vm_paddr_t> vm_aspace::unmap_page(uintptr_t vaddr) {
    if (m_arch.root_phys == 0) { return ktl::nothing; }
    if (!is_canonical(vaddr)) { return ktl::nothing; }
    if ((vaddr & 0xFFF) != 0) { return ktl::nothing; }

    // 4K only: large mappings are bootloader-owned kernel mappings and are
    // never torn down through this path.
    uint64_t* leaf = find_leaf_slot(m_arch.root_phys, vaddr);
    if (leaf == nullptr) { return ktl::nothing; }
    if (!arch::pte_present(*leaf)) { return ktl::nothing; }

    vm_paddr_t paddr = arch::pte_addr(*leaf);
    *leaf            = 0;
    arch::flush_tlb_page(m_arch.root_phys, vaddr);
    return paddr;
}

void vm_aspace::activate() {
    arch::set_root(m_arch.root_phys);
    g_active_space = this;
}

vm_aspace* vm_aspace::active() { return g_active_space; }

// End of the canonical low half: bit 47 sign-extension boundary.
uintptr_t vm_aspace::low_limit() { return 0x0000800000000000ull; }

}  // namespace kernel::mm
