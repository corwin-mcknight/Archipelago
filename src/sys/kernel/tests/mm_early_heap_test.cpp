#include <stddef.h>
#include <stdint.h>

#include "kernel/mm/early_heap.h"
#include "kernel/testing/testing.h"

namespace {

constexpr size_t shared_heap_size = 4096;
alignas(max_align_t) uint8_t g_shared_heap_storage[shared_heap_size];
kernel::mm::early_heap g_test_heap;

void reset_shared_heap(size_t bytes) {
    uintptr_t start = reinterpret_cast<uintptr_t>(g_shared_heap_storage);
    g_test_heap.on_boot(start, start + bytes);
}

}  // namespace

KTEST_WITH_INIT(early_heap_alloc_free_reuses_block, "mm/early_heap", early_heap_init_512) {
    void *first = g_test_heap.alloc(64, 16);
    KTEST_REQUIRE_TRUE(first != nullptr);
    KTEST_EXPECT_EQUAL(reinterpret_cast<uintptr_t>(first) & (16 - 1), static_cast<uintptr_t>(0));

    g_test_heap.free(first);

    void *second = g_test_heap.alloc(64, 16);
    KTEST_REQUIRE_TRUE(second != nullptr);
    KTEST_EXPECT_TRUE(second == first);
}

KTEST_WITH_INIT(early_heap_respects_large_alignment, "mm/early_heap", early_heap_init_512) {
    constexpr size_t alignment = 128;
    void *ptr                  = g_test_heap.alloc(48, alignment);

    KTEST_REQUIRE_TRUE(ptr != nullptr);
    KTEST_EXPECT_EQUAL(reinterpret_cast<uintptr_t>(ptr) & (alignment - 1), static_cast<uintptr_t>(0));
}

KTEST_WITH_INIT(early_heap_alignment_falls_back_to_max_align, "mm/early_heap", early_heap_init_512) {
    const size_t fallback_alignment = alignof(max_align_t);

    void *ptr                       = g_test_heap.alloc(24, 1);
    KTEST_REQUIRE_TRUE(ptr != nullptr);
    KTEST_EXPECT_EQUAL(reinterpret_cast<uintptr_t>(ptr) & (fallback_alignment - 1), static_cast<uintptr_t>(0));
}

KTEST_WITH_INIT(early_heap_zero_size_returns_null, "mm/early_heap", early_heap_init_512) {
    void *nothing = g_test_heap.alloc(0, 16);
    KTEST_EXPECT_TRUE(nothing == nullptr);

    void *usable = g_test_heap.alloc(64, 16);
    KTEST_REQUIRE_TRUE(usable != nullptr);
}

KTEST_WITH_INIT(early_heap_split_reuses_head_space, "mm/early_heap", early_heap_init_512) {
    void *first  = g_test_heap.alloc(64, 16);
    void *second = g_test_heap.alloc(32, 16);
    KTEST_REQUIRE_TRUE(first != nullptr);
    KTEST_REQUIRE_TRUE(second != nullptr);

    g_test_heap.free(first);

    void *third = g_test_heap.alloc(32, 16);
    KTEST_REQUIRE_TRUE(third != nullptr);
    KTEST_EXPECT_TRUE(third == first);
    KTEST_EXPECT_TRUE(third != second);
}

KTEST_WITH_INIT(early_heap_coalesces_adjacent_blocks, "mm/early_heap", early_heap_init_full) {
    void *first  = g_test_heap.alloc(64, 16);
    void *second = g_test_heap.alloc(64, 16);
    void *third  = g_test_heap.alloc(64, 16);

    KTEST_REQUIRE_TRUE(first != nullptr);
    KTEST_REQUIRE_TRUE(second != nullptr);
    KTEST_REQUIRE_TRUE(third != nullptr);

    g_test_heap.free(second);
    g_test_heap.free(first);

    void *merged = g_test_heap.alloc(128, 16);
    KTEST_REQUIRE_TRUE(merged != nullptr);
    KTEST_EXPECT_TRUE(merged == first);

    g_test_heap.free(third);
    g_test_heap.free(merged);

    void *large = g_test_heap.alloc(160, 16);
    KTEST_REQUIRE_TRUE(large != nullptr);
    KTEST_EXPECT_TRUE(large == first);
}

KTEST_WITH_INIT(early_heap_supports_multiple_alignment_requests, "mm/early_heap", early_heap_init_full) {
    constexpr size_t alignments[]                               = {1, 2, 4, 8, 16, 32, 64, 128};
    void *allocated[sizeof(alignments) / sizeof(alignments[0])] = {nullptr};

    for (size_t i = 0; i < sizeof(alignments) / sizeof(alignments[0]); ++i) {
        allocated[i] = g_test_heap.alloc(48, alignments[i]);
        KTEST_REQUIRE_TRUE(allocated[i] != nullptr);
        KTEST_EXPECT_EQUAL(reinterpret_cast<uintptr_t>(allocated[i]) & (alignments[i] - 1), static_cast<uintptr_t>(0));
    }

    for (size_t i = 0; i < sizeof(alignments) / sizeof(alignments[0]); ++i) { g_test_heap.free(allocated[i]); }

    void *merged = g_test_heap.alloc(256, alignof(max_align_t));
    KTEST_REQUIRE_TRUE(merged != nullptr);
    KTEST_EXPECT_TRUE(merged == allocated[0]);
}

KTEST_WITH_INIT(early_heap_many_small_allocations_stress, "mm/early_heap", early_heap_init_full) {
    constexpr size_t block_count = 64;
    void *blocks[block_count]    = {nullptr};

    for (size_t i = 0; i < block_count; ++i) {
        blocks[i] = g_test_heap.alloc(8, 8);
        KTEST_REQUIRE_TRUE(blocks[i] != nullptr);
        if (i > 0) {
            KTEST_EXPECT_TRUE(reinterpret_cast<uintptr_t>(blocks[i]) > reinterpret_cast<uintptr_t>(blocks[i - 1]));
        }
        KTEST_EXPECT_EQUAL(reinterpret_cast<uintptr_t>(blocks[i]) & (8 - 1), static_cast<uintptr_t>(0));
    }

    for (size_t i = 0; i < block_count; i += 2) { g_test_heap.free(blocks[i]); }
    for (size_t i = 1; i < block_count; i += 2) { g_test_heap.free(blocks[i]); }

    void *large = g_test_heap.alloc(shared_heap_size / 2, alignof(max_align_t));
    KTEST_REQUIRE_TRUE(large != nullptr);
    KTEST_EXPECT_TRUE(large == blocks[0]);
}

KTEST_WITH_INIT(early_heap_reinitialization_cycles, "mm/early_heap", early_heap_init_full) {
    constexpr size_t cycle_count = 10;

    for (size_t cycle = 0; cycle < cycle_count; ++cycle) {
        reset_shared_heap(512);

        void *a = g_test_heap.alloc(64, 16);
        void *b = g_test_heap.alloc(96, 32);
        void *c = g_test_heap.alloc(48, 8);

        KTEST_REQUIRE_TRUE(a != nullptr);
        KTEST_REQUIRE_TRUE(b != nullptr);
        KTEST_REQUIRE_TRUE(c != nullptr);

        g_test_heap.free(b);
        g_test_heap.free(a);

        void *big = g_test_heap.alloc(120, 16);
        KTEST_REQUIRE_TRUE(big != nullptr);
        KTEST_EXPECT_TRUE(big == a);

        g_test_heap.free(c);
        g_test_heap.free(big);
    }
}

KTEST_WITH_INIT(early_heap_free_null_is_noop, "mm/early_heap", early_heap_init_128) { g_test_heap.free(nullptr); }

static void early_heap_init_128() { reset_shared_heap(128); }
static void early_heap_init_512() { reset_shared_heap(512); }
static void early_heap_init_full() { reset_shared_heap(shared_heap_size); }
