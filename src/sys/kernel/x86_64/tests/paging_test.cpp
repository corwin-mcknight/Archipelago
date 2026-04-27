#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/mm/paging.h>
#include <kernel/mm/pmm.h>

namespace {

constexpr uintptr_t low_vaddr  = 0x100000;        // 1 MiB
constexpr uintptr_t mid_vaddr  = 0x40000000;      // 1 GiB -- different PDPT entry
constexpr uintptr_t high_vaddr = 0x10000000000;   // 1 TiB -- different PML4 entry

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

    KTEST_REQUIRE_TRUE(space.map_page(low_vaddr,  p1, kernel::mm::pte::WRITABLE));
    KTEST_REQUIRE_TRUE(space.map_page(mid_vaddr,  p2, kernel::mm::pte::WRITABLE));
    KTEST_REQUIRE_TRUE(space.map_page(high_vaddr, p3, kernel::mm::pte::WRITABLE));

    KTEST_EXPECT_VALUE(space.walk(low_vaddr),  p1);
    KTEST_EXPECT_VALUE(space.walk(mid_vaddr),  p2);
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

#endif  // CONFIG_KERNEL_TESTING
