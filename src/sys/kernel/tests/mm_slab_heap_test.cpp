#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/mm/slab_heap.h>

#include <ktl/vector>

using namespace kernel::mm;

KTEST_MODULE("mm/slab_heap");

// Fork-per-test isolation means each case sees g_slab_heap fresh; stats deltas within a case are
// therefore exact. Pages come from the host runner's aligned_alloc stub, so ASan watches the
// slab arithmetic.

// One object through its life: the right class grows by one slab, the pointer is aligned and
// usable, and freeing empties the class but keeps the slab as the hot spare.
KTEST_CASE(slab_heap_single_object_lifecycle) {
    auto before = g_slab_heap.stats();
    void* p     = g_slab_heap.alloc(24);
    KTEST_REQUIRE_TRUE(p != nullptr);
    KTEST_EXPECT_TRUE(reinterpret_cast<uintptr_t>(p) % 16 == 0);
    __builtin_memset(p, 0xAB, 24);

    auto during = g_slab_heap.stats();
    KTEST_EXPECT_ALL(during.classes[1].object_size == 32, during.classes[1].live_objects == 1,
                     during.classes[1].slabs == before.classes[1].slabs + 1);

    g_slab_heap.free(p);
    auto after = g_slab_heap.stats();
    KTEST_EXPECT_ALL(after.classes[1].live_objects == 0, after.classes[1].slabs == 1,
                     after.free_calls == before.free_calls + 1);
}

// Overflowing one slab grows a second; freeing everything releases it back down to the single
// spare. The slot count comes from the stats so the test tracks the real layout.
KTEST_CASE(slab_heap_grows_and_shrinks) {
    void* first = g_slab_heap.alloc(64);
    KTEST_REQUIRE_TRUE(first != nullptr);
    size_t per_slab = g_slab_heap.stats().classes[2].free_slots + 1;

    ktl::vector<void*> held;
    KTEST_REQUIRE_TRUE(held.push_back(first));
    for (size_t i = 1; i < per_slab + 1; ++i) {
        void* p = g_slab_heap.alloc(64);
        KTEST_REQUIRE_TRUE(p != nullptr);
        KTEST_REQUIRE_TRUE(held.push_back(p));
    }
    KTEST_EXPECT_TRUE(g_slab_heap.stats().classes[2].slabs == 2);

    for (size_t i = 0; i < held.size(); ++i) { g_slab_heap.free(held[i]); }
    auto after = g_slab_heap.stats();
    KTEST_EXPECT_ALL(after.classes[2].live_objects == 0, after.classes[2].slabs == 1);
}

// Freed slots are reused before fresh ones: a full free/realloc cycle of a small batch stays
// within one slab.
KTEST_CASE(slab_heap_reuses_freed_slots) {
    void* a = g_slab_heap.alloc(100);
    void* b = g_slab_heap.alloc(100);
    KTEST_REQUIRE_TRUE(a != nullptr && b != nullptr);
    g_slab_heap.free(a);
    g_slab_heap.free(b);
    void* c = g_slab_heap.alloc(100);
    void* d = g_slab_heap.alloc(100);
    KTEST_EXPECT_TRUE((c == a || c == b) && (d == a || d == b) && c != d);
    KTEST_EXPECT_TRUE(g_slab_heap.stats().classes[3].slabs == 1);
    g_slab_heap.free(c);
    g_slab_heap.free(d);
}

// Above the largest class the heap hands out bare page runs: page-aligned (the run length lives
// in metadata beside the pages, not in a header inside them), sized to exactly the pages the
// request needs, fully writable, and returned page-accounted.
KTEST_CASE(slab_heap_large_allocations) {
    auto before = g_slab_heap.stats();
    void* p     = g_slab_heap.alloc(5000);
    KTEST_REQUIRE_TRUE(p != nullptr);
    KTEST_EXPECT_TRUE(reinterpret_cast<uintptr_t>(p) % 4096 == 0);
    __builtin_memset(p, 0xCD, 5000);

    auto during = g_slab_heap.stats();
    KTEST_EXPECT_ALL(during.large_allocs == before.large_allocs + 1, during.large_pages == before.large_pages + 2);

    g_slab_heap.free(p);
    auto after = g_slab_heap.stats();
    KTEST_EXPECT_ALL(after.large_allocs == before.large_allocs, after.large_pages == before.large_pages);

    // A page-sized request costs exactly one page -- the case an in-page header would double.
    void* whole = g_slab_heap.alloc(4096);
    KTEST_REQUIRE_TRUE(whole != nullptr);
    __builtin_memset(whole, 0xEF, 4096);
    KTEST_EXPECT_TRUE(g_slab_heap.stats().large_pages == before.large_pages + 1);
    g_slab_heap.free(whole);
}

// The alignment argument is honored beyond the class's natural alignment: a stricter alignment
// than size promotes the allocation to a class that can deliver it, and past the slab cap the
// large path delivers natural page alignment even for tiny sizes.
KTEST_CASE(slab_heap_alignment) {
    void* p = g_slab_heap.alloc(8, 64);
    KTEST_REQUIRE_TRUE(p != nullptr);
    KTEST_EXPECT_TRUE(reinterpret_cast<uintptr_t>(p) % 64 == 0);
    KTEST_EXPECT_TRUE(g_slab_heap.stats().classes[2].live_objects == 1);
    g_slab_heap.free(p);

    void* page = g_slab_heap.alloc(16, 4096);
    KTEST_REQUIRE_TRUE(page != nullptr);
    KTEST_EXPECT_TRUE(reinterpret_cast<uintptr_t>(page) % 4096 == 0);
    KTEST_EXPECT_TRUE(g_slab_heap.stats().large_allocs == 1);
    g_slab_heap.free(page);

    void* q = g_slab_heap.alloc(0);
    KTEST_REQUIRE_TRUE(q != nullptr);
    g_slab_heap.free(q);
}

// Adversarial sizes fail as exhaustion, never as success: page rounding that would wrap, and
// page counts past what the descriptor's 32-bit run field records, both come back nullptr with
// the failure counted.
KTEST_CASE(slab_heap_adversarial_sizes_fail_cleanly) {
    auto before   = g_slab_heap.stats();

    void* wrapped = g_slab_heap.alloc(SIZE_MAX - 7);
    KTEST_EXPECT_TRUE(wrapped == nullptr);
    void* max = g_slab_heap.alloc(SIZE_MAX);
    KTEST_EXPECT_TRUE(max == nullptr);
    void* wide = g_slab_heap.alloc(1ull << 45);  // 2^33 pages: exceeds the 32-bit run field
    KTEST_EXPECT_TRUE(wide == nullptr);

    auto after = g_slab_heap.stats();
    KTEST_EXPECT_ALL(after.failures == before.failures + 3, after.large_allocs == before.large_allocs,
                     after.large_pages == before.large_pages);
}

// Mixed-size churn with interleaved frees: everything lands back at zero live objects and every
// slab beyond the per-class spare is returned.
KTEST_CASE(slab_heap_mixed_churn) {
    constexpr size_t SIZES[] = {8, 24, 60, 130, 500, 1000, 3000};
    ktl::vector<void*> held;
    for (size_t round = 0; round < 50; ++round) {
        for (size_t s = 0; s < sizeof(SIZES) / sizeof(SIZES[0]); ++s) {
            void* p = g_slab_heap.alloc(SIZES[s]);
            KTEST_REQUIRE_TRUE(p != nullptr);
            __builtin_memset(p, static_cast<int>(round), SIZES[s]);
            KTEST_REQUIRE_TRUE(held.push_back(p));
        }
        if (round % 3 == 0) {
            for (size_t i = 0; i < 3 && !held.empty(); ++i) {
                g_slab_heap.free(held.back().value());
                (void)held.pop_back();
            }
        }
    }
    for (size_t i = 0; i < held.size(); ++i) { g_slab_heap.free(held[i]); }

    auto after  = g_slab_heap.stats();
    size_t live = after.large_allocs;
    for (const auto& cls : after.classes) {
        live += cls.live_objects;
        KTEST_EXPECT_TRUE(cls.slabs <= 1);
    }
    KTEST_EXPECT_TRUE(live == 0);
}

#endif
