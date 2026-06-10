#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/mm/paging.h>
#include <kernel/mm/pmm.h>

extern uintptr_t g_hhdm_offset;

namespace {

constexpr uintptr_t low_vaddr  = 0x100000;       // 1 MiB
constexpr uintptr_t mid_vaddr  = 0x40000000;     // 1 GiB -- different PDPT entry
constexpr uintptr_t high_vaddr = 0x10000000000;  // 1 TiB -- different PML4 entry

uint64_t table_entry(kernel::mm::vm_paddr_t table_phys, size_t index) {
    return reinterpret_cast<const uint64_t*>(table_phys + g_hhdm_offset)[index];
}

}  // namespace

KTEST(paging_init_destroy_roundtrip, "x86_64/paging") {
    kernel::mm::address_space space;
    KTEST_REQUIRE_TRUE(space.init());
    KTEST_EXPECT_TRUE(space.is_valid());
    KTEST_EXPECT_NOT_EQUAL(space.pml4_phys(), static_cast<size_t>(0));

    space.destroy();
    KTEST_EXPECT_FALSE(space.is_valid());
    KTEST_EXPECT_EQUAL(space.pml4_phys(), static_cast<size_t>(0));
}

KTEST(paging_double_init_fails, "x86_64/paging") {
    kernel::mm::address_space space;
    KTEST_REQUIRE_TRUE(space.init());
    KTEST_EXPECT_FALSE(space.init());
}

KTEST(paging_walk_unmapped_returns_nothing, "x86_64/paging") {
    kernel::mm::address_space space;
    KTEST_REQUIRE_TRUE(space.init());
    KTEST_EXPECT_FALSE(space.walk(low_vaddr).has_value());
}

KTEST(paging_map_walk_roundtrip, "x86_64/paging") {
    kernel::mm::address_space space;
    KTEST_REQUIRE_TRUE(space.init());

    KTEST_REQUIRE_VALUE(leaf, kernel::mm::g_page_frame_allocator.alloc());

    KTEST_REQUIRE_TRUE(space.map_page(low_vaddr, leaf, kernel::mm::pte::WRITABLE));
    KTEST_EXPECT_VALUE(space.walk(low_vaddr), leaf);

    kernel::mm::g_page_frame_allocator.free(leaf);
}

KTEST(paging_walk_returns_paddr_with_offset, "x86_64/paging") {
    kernel::mm::address_space space;
    KTEST_REQUIRE_TRUE(space.init());

    KTEST_REQUIRE_VALUE(leaf, kernel::mm::g_page_frame_allocator.alloc());
    KTEST_REQUIRE_TRUE(space.map_page(low_vaddr, leaf, kernel::mm::pte::WRITABLE));

    KTEST_EXPECT_VALUE(space.walk(low_vaddr + 0x123), leaf + 0x123);

    kernel::mm::g_page_frame_allocator.free(leaf);
}

KTEST(paging_double_map_fails, "x86_64/paging") {
    kernel::mm::address_space space;
    KTEST_REQUIRE_TRUE(space.init());

    KTEST_REQUIRE_VALUE(leaf, kernel::mm::g_page_frame_allocator.alloc());

    KTEST_REQUIRE_TRUE(space.map_page(low_vaddr, leaf, kernel::mm::pte::WRITABLE));
    KTEST_EXPECT_FALSE(space.map_page(low_vaddr, leaf, kernel::mm::pte::WRITABLE));

    kernel::mm::g_page_frame_allocator.free(leaf);
}

KTEST(paging_unmap_clears_mapping, "x86_64/paging") {
    kernel::mm::address_space space;
    KTEST_REQUIRE_TRUE(space.init());

    KTEST_REQUIRE_VALUE(leaf, kernel::mm::g_page_frame_allocator.alloc());
    KTEST_REQUIRE_TRUE(space.map_page(low_vaddr, leaf, kernel::mm::pte::WRITABLE));

    KTEST_EXPECT_VALUE(space.unmap_page(low_vaddr), leaf);
    KTEST_EXPECT_FALSE(space.walk(low_vaddr).has_value());

    kernel::mm::g_page_frame_allocator.free(leaf);
}

KTEST(paging_unmap_unmapped_returns_nothing, "x86_64/paging") {
    kernel::mm::address_space space;
    KTEST_REQUIRE_TRUE(space.init());
    KTEST_EXPECT_FALSE(space.unmap_page(low_vaddr).has_value());
}

KTEST(paging_map_unaligned_fails, "x86_64/paging") {
    kernel::mm::address_space space;
    KTEST_REQUIRE_TRUE(space.init());

    KTEST_REQUIRE_VALUE(leaf, kernel::mm::g_page_frame_allocator.alloc());

    KTEST_EXPECT_FALSE(space.map_page(low_vaddr + 1, leaf, kernel::mm::pte::WRITABLE));
    KTEST_EXPECT_FALSE(space.map_page(low_vaddr, leaf + 1, kernel::mm::pte::WRITABLE));

    kernel::mm::g_page_frame_allocator.free(leaf);
}

KTEST(paging_independent_mappings_across_levels, "x86_64/paging") {
    kernel::mm::address_space space;
    KTEST_REQUIRE_TRUE(space.init());

    KTEST_REQUIRE_VALUE(p1, kernel::mm::g_page_frame_allocator.alloc());
    KTEST_REQUIRE_VALUE(p2, kernel::mm::g_page_frame_allocator.alloc());
    KTEST_REQUIRE_VALUE(p3, kernel::mm::g_page_frame_allocator.alloc());

    KTEST_REQUIRE_TRUE(space.map_page(low_vaddr, p1, kernel::mm::pte::WRITABLE));
    KTEST_REQUIRE_TRUE(space.map_page(mid_vaddr, p2, kernel::mm::pte::WRITABLE));
    KTEST_REQUIRE_TRUE(space.map_page(high_vaddr, p3, kernel::mm::pte::WRITABLE));

    KTEST_EXPECT_VALUE(space.walk(low_vaddr), p1);
    KTEST_EXPECT_VALUE(space.walk(mid_vaddr), p2);
    KTEST_EXPECT_VALUE(space.walk(high_vaddr), p3);

    kernel::mm::g_page_frame_allocator.free(p1);
    kernel::mm::g_page_frame_allocator.free(p2);
    kernel::mm::g_page_frame_allocator.free(p3);
}

KTEST(paging_destroy_invalidates_walks, "x86_64/paging") {
    kernel::mm::address_space space;
    KTEST_REQUIRE_TRUE(space.init());

    KTEST_REQUIRE_VALUE(leaf, kernel::mm::g_page_frame_allocator.alloc());
    KTEST_REQUIRE_TRUE(space.map_page(low_vaddr, leaf, kernel::mm::pte::WRITABLE));

    space.destroy();
    KTEST_EXPECT_FALSE(space.walk(low_vaddr).has_value());

    kernel::mm::g_page_frame_allocator.free(leaf);
}

KTEST(paging_map_no_execute_rejected, "x86_64/paging") {
    kernel::mm::address_space space;
    KTEST_REQUIRE_TRUE(space.init());

    KTEST_REQUIRE_VALUE(leaf, kernel::mm::g_page_frame_allocator.alloc());

    // NO_EXECUTE is rejected until EFER.NXE is enabled -- bit 63 is reserved
    // in a present PTE while NXE is clear.
    KTEST_EXPECT_FALSE(space.map_page(low_vaddr, leaf, kernel::mm::pte::WRITABLE | kernel::mm::pte::NO_EXECUTE));
    KTEST_EXPECT_FALSE(space.walk(low_vaddr).has_value());

    kernel::mm::g_page_frame_allocator.free(leaf);
}

KTEST(paging_map_masks_unsupported_flags, "x86_64/paging") {
    kernel::mm::address_space space;
    KTEST_REQUIRE_TRUE(space.init());

    KTEST_REQUIRE_VALUE(leaf, kernel::mm::g_page_frame_allocator.alloc());

    // HUGE makes no sense on a 4 KiB leaf; it must be masked out, not written.
    KTEST_REQUIRE_TRUE(space.map_page(low_vaddr, leaf, kernel::mm::pte::WRITABLE | kernel::mm::pte::HUGE));
    KTEST_EXPECT_VALUE(space.walk(low_vaddr), leaf);

    uint64_t pml4e    = table_entry(space.pml4_phys(), (low_vaddr >> 39) & 0x1FF);
    uint64_t pdpte    = table_entry(pml4e & kernel::mm::pte::ADDR_MASK, (low_vaddr >> 30) & 0x1FF);
    uint64_t pde      = table_entry(pdpte & kernel::mm::pte::ADDR_MASK, (low_vaddr >> 21) & 0x1FF);
    uint64_t leaf_pte = table_entry(pde & kernel::mm::pte::ADDR_MASK, (low_vaddr >> 12) & 0x1FF);
    KTEST_EXPECT_TRUE((leaf_pte & kernel::mm::pte::HUGE) == 0);

    kernel::mm::g_page_frame_allocator.free(leaf);
}

KTEST(paging_user_widens_existing_intermediates, "x86_64/paging") {
    kernel::mm::address_space space;
    KTEST_REQUIRE_TRUE(space.init());

    KTEST_REQUIRE_VALUE(kpage, kernel::mm::g_page_frame_allocator.alloc());
    KTEST_REQUIRE_VALUE(upage, kernel::mm::g_page_frame_allocator.alloc());

    // A kernel-only mapping creates the intermediate tables without USER.
    KTEST_REQUIRE_TRUE(space.map_page(low_vaddr, kpage, kernel::mm::pte::WRITABLE));

    uint64_t pml4e = table_entry(space.pml4_phys(), (low_vaddr >> 39) & 0x1FF);
    KTEST_EXPECT_TRUE((pml4e & kernel::mm::pte::USER) == 0);

    // A user mapping sharing the same PML4/PDPT/PD entries must widen USER
    // into every pre-existing level -- x86_64 requires it at each level.
    constexpr uintptr_t user_vaddr = low_vaddr + 0x1000;
    KTEST_REQUIRE_TRUE(space.map_page(user_vaddr, upage, kernel::mm::pte::WRITABLE | kernel::mm::pte::USER));

    pml4e = table_entry(space.pml4_phys(), (user_vaddr >> 39) & 0x1FF);
    KTEST_EXPECT_TRUE((pml4e & kernel::mm::pte::USER) != 0);

    uint64_t pdpte = table_entry(pml4e & kernel::mm::pte::ADDR_MASK, (user_vaddr >> 30) & 0x1FF);
    KTEST_EXPECT_TRUE((pdpte & kernel::mm::pte::USER) != 0);

    uint64_t pde = table_entry(pdpte & kernel::mm::pte::ADDR_MASK, (user_vaddr >> 21) & 0x1FF);
    KTEST_EXPECT_TRUE((pde & kernel::mm::pte::USER) != 0);

    uint64_t user_leaf = table_entry(pde & kernel::mm::pte::ADDR_MASK, (user_vaddr >> 12) & 0x1FF);
    KTEST_EXPECT_TRUE((user_leaf & kernel::mm::pte::USER) != 0);

    // The kernel-only leaf sharing the same PT stays kernel-only.
    uint64_t kernel_leaf = table_entry(pde & kernel::mm::pte::ADDR_MASK, (low_vaddr >> 12) & 0x1FF);
    KTEST_EXPECT_TRUE((kernel_leaf & kernel::mm::pte::USER) == 0);

    kernel::mm::g_page_frame_allocator.free(kpage);
    kernel::mm::g_page_frame_allocator.free(upage);
}

#endif  // CONFIG_KERNEL_TESTING
