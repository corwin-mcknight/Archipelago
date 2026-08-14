#include <kernel/mm/early_heap.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/slab_heap.h>
#include <kernel/testing/testing.h>
#include <std/new.h>

#include <ktl/vector>

using namespace kernel::mm;

KTEST_MODULE("mm/slab_heap_boot");

// The switchover happened during boot: operator new now lands in the slab heap (visible as an
// alloc-call delta), the pointer lies outside the early heap's region, and delete routes back to
// the slab. This is the contract mm/heap.cpp's routing promises.
KTEST_CASE(heap_operator_new_routes_to_slab) {
    KTEST_REQUIRE_TRUE(heap_slab_active());

    auto before = g_slab_heap.stats();
    auto* value = new (std::nothrow) uint64_t(0x51ABu);
    KTEST_REQUIRE_TRUE(value != nullptr);
    KTEST_EXPECT_TRUE(!g_early_heap.contains(value));
    KTEST_EXPECT_TRUE(g_slab_heap.stats().alloc_calls == before.alloc_calls + 1);

    delete value;
    KTEST_EXPECT_TRUE(g_slab_heap.stats().free_calls == before.free_calls + 1);
}

// Churn against the real PMM: pages borrowed for slabs and large runs all return, so the PMM's
// free-page count recovers to within the per-class spare slabs the heap deliberately keeps.
KTEST_CASE(heap_pages_return_to_pmm) {
    size_t free_before = g_page_frame_allocator.free_pages();

    {
        ktl::vector<void*> held;
        for (size_t round = 0; round < 20; ++round) {
            KTEST_REQUIRE_TRUE(held.push_back(g_slab_heap.alloc(48)));
            KTEST_REQUIRE_TRUE(held.push_back(g_slab_heap.alloc(700)));
            KTEST_REQUIRE_TRUE(held.push_back(g_slab_heap.alloc(9000)));
        }
        for (size_t i = 0; i < held.size(); ++i) {
            KTEST_REQUIRE_TRUE(held[i] != nullptr);
            g_slab_heap.free(held[i]);
        }
    }

    size_t free_after = g_page_frame_allocator.free_pages();
    // The spares (at most one page per class) may or may not have existed before this test ran,
    // and unrelated kernel threads allocate concurrently -- allow a small drift, not a leak.
    KTEST_EXPECT_TRUE(free_before <= free_after + slab_heap::CLASS_COUNT + 4);
}
