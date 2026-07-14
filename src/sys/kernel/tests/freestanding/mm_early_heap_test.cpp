#include <stddef.h>
#include <stdint.h>

#include "kernel/mm/early_heap.h"
#include "kernel/testing/testing.h"

// Early-heap tests run against a private heap over a static buffer, so each
// test (and each phase within a merged story) resets the heap explicitly via
// on_boot before exercising it. The stories: free-list reuse/split/coalesce,
// alignment behavior, degenerate inputs, a stress pass, re-initialization
// cycles, and stats/introspection.

KTEST_MODULE("mm/early_heap");

namespace {

constexpr size_t shared_heap_size = 4096;
alignas(max_align_t) uint8_t g_shared_heap_storage[shared_heap_size];
kernel::mm::early_heap g_test_heap;

void reset_shared_heap(size_t bytes) {
    uintptr_t start = reinterpret_cast<uintptr_t>(g_shared_heap_storage);
    g_test_heap.on_boot(start, start + bytes);
}

}  // namespace

// Story: the free list recycles memory -- a freed block is reused, a freed
// head block satisfies a smaller allocation without disturbing its neighbor,
// and adjacent free blocks coalesce into larger ones.
KTEST_CASE(early_heap_reuse_split_and_coalesce) {
    // Phase 1: alloc/free/alloc reuses the same block.
    reset_shared_heap(512);
    {
        void* first = g_test_heap.alloc(64, 16);
        KTEST_REQUIRE_TRUE(first != nullptr);
        KTEST_EXPECT_ALIGNED(first, 16);
        g_test_heap.free(first);

        void* second = g_test_heap.alloc(64, 16);
        KTEST_REQUIRE_TRUE(second != nullptr);
        KTEST_EXPECT_TRUE(second == first);
    }

    // Phase 2: a freed head block is split and reused ahead of its neighbor.
    reset_shared_heap(512);
    {
        void* first  = g_test_heap.alloc(64, 16);
        void* second = g_test_heap.alloc(32, 16);
        KTEST_REQUIRE_TRUE(first != nullptr);
        KTEST_REQUIRE_TRUE(second != nullptr);
        g_test_heap.free(first);

        void* third = g_test_heap.alloc(32, 16);
        KTEST_REQUIRE_TRUE(third != nullptr);
        KTEST_EXPECT_ALL(third == first, third != second);
    }

    // Phase 3: adjacent free blocks coalesce.
    reset_shared_heap(shared_heap_size);
    {
        void* first  = g_test_heap.alloc(64, 16);
        void* second = g_test_heap.alloc(64, 16);
        void* third  = g_test_heap.alloc(64, 16);
        KTEST_REQUIRE_TRUE(first != nullptr);
        KTEST_REQUIRE_TRUE(second != nullptr);
        KTEST_REQUIRE_TRUE(third != nullptr);

        g_test_heap.free(second);
        g_test_heap.free(first);

        void* merged = g_test_heap.alloc(128, 16);
        KTEST_REQUIRE_TRUE(merged != nullptr);
        KTEST_EXPECT_TRUE(merged == first);

        g_test_heap.free(third);
        g_test_heap.free(merged);

        void* large = g_test_heap.alloc(160, 16);
        KTEST_REQUIRE_TRUE(large != nullptr);
        KTEST_EXPECT_TRUE(large == first);
    }
}

// Story: alignment behavior -- large alignments are honored, sub-minimum
// requests fall back to max_align_t, and a mixed run of alignments all land
// correctly and coalesce back afterwards.
KTEST_CASE(early_heap_alignment) {
    // Phase 1: a large alignment request is honored.
    reset_shared_heap(512);
    {
        void* ptr = g_test_heap.alloc(48, 128);
        KTEST_REQUIRE_TRUE(ptr != nullptr);
        KTEST_EXPECT_ALIGNED(ptr, 128);
    }

    // Phase 2: tiny alignment falls back to max_align_t.
    reset_shared_heap(512);
    {
        void* ptr = g_test_heap.alloc(24, 1);
        KTEST_REQUIRE_TRUE(ptr != nullptr);
        KTEST_EXPECT_ALIGNED(ptr, alignof(max_align_t));
    }

    // Phase 3: a mix of alignment requests all land aligned, and freeing them
    // coalesces back to one block.
    reset_shared_heap(shared_heap_size);
    {
        constexpr size_t alignments[]                               = {1, 2, 4, 8, 16, 32, 64, 128};
        void* allocated[sizeof(alignments) / sizeof(alignments[0])] = {nullptr};

        for (size_t i = 0; i < sizeof(alignments) / sizeof(alignments[0]); ++i) {
            allocated[i] = g_test_heap.alloc(48, alignments[i]);
            KTEST_REQUIRE_TRUE(allocated[i] != nullptr);
            KTEST_EXPECT_ALIGNED(allocated[i], alignments[i]);
        }

        for (size_t i = 0; i < sizeof(alignments) / sizeof(alignments[0]); ++i) { g_test_heap.free(allocated[i]); }

        void* merged = g_test_heap.alloc(256, alignof(max_align_t));
        KTEST_REQUIRE_TRUE(merged != nullptr);
        KTEST_EXPECT_TRUE(merged == allocated[0]);
    }
}

// Story: degenerate inputs -- freeing null is a no-op and a zero-size alloc
// returns null without poisoning the heap.
KTEST_CASE(early_heap_degenerate_inputs) {
    reset_shared_heap(512);
    g_test_heap.free(nullptr);
    KTEST_EXPECT_TRUE(g_test_heap.alloc(0, 16) == nullptr);
    KTEST_REQUIRE_TRUE(g_test_heap.alloc(64, 16) != nullptr);
}

KTEST_CASE(early_heap_many_small_allocations_stress) {
    reset_shared_heap(shared_heap_size);

    constexpr size_t block_count = 64;
    void* blocks[block_count]    = {nullptr};

    for (size_t i = 0; i < block_count; ++i) {
        blocks[i] = g_test_heap.alloc(8, 8);
        KTEST_REQUIRE_TRUE(blocks[i] != nullptr);
        if (i > 0) {
            KTEST_EXPECT_TRUE(reinterpret_cast<uintptr_t>(blocks[i]) > reinterpret_cast<uintptr_t>(blocks[i - 1]));
        }
        KTEST_EXPECT_ALIGNED(blocks[i], 8);
    }

    for (size_t i = 0; i < block_count; i += 2) { g_test_heap.free(blocks[i]); }
    for (size_t i = 1; i < block_count; i += 2) { g_test_heap.free(blocks[i]); }

    void* large = g_test_heap.alloc(shared_heap_size / 2, alignof(max_align_t));
    KTEST_REQUIRE_TRUE(large != nullptr);
    KTEST_EXPECT_TRUE(large == blocks[0]);
}

KTEST_CASE(early_heap_reinitialization_cycles) {
    constexpr size_t cycle_count = 10;

    for (size_t cycle = 0; cycle < cycle_count; ++cycle) {
        reset_shared_heap(512);

        void* a = g_test_heap.alloc(64, 16);
        void* b = g_test_heap.alloc(96, 32);
        void* c = g_test_heap.alloc(48, 8);
        KTEST_REQUIRE_TRUE(a != nullptr);
        KTEST_REQUIRE_TRUE(b != nullptr);
        KTEST_REQUIRE_TRUE(c != nullptr);

        g_test_heap.free(b);
        g_test_heap.free(a);

        void* big = g_test_heap.alloc(120, 16);
        KTEST_REQUIRE_TRUE(big != nullptr);
        KTEST_EXPECT_TRUE(big == a);

        g_test_heap.free(c);
        g_test_heap.free(big);
    }
}

// Story: introspection -- the stats counters track usage and peak, and the
// block walk agrees with the stats totals.
KTEST_CASE(early_heap_stats_and_block_walk) {
    // Phase 1: counters track usage and the peak high-water mark.
    reset_shared_heap(512);
    {
        auto fresh = g_test_heap.stats();
        KTEST_EXPECT_EQUAL(fresh.used_bytes, static_cast<size_t>(0));
        KTEST_EXPECT_EQUAL(fresh.alloc_calls, static_cast<uint64_t>(0));

        void* a = g_test_heap.alloc(64, 16);
        void* b = g_test_heap.alloc(96, 16);
        KTEST_REQUIRE_TRUE(a != nullptr);
        KTEST_REQUIRE_TRUE(b != nullptr);

        auto loaded = g_test_heap.stats();
        KTEST_EXPECT_EQUAL(loaded.alloc_calls, static_cast<uint64_t>(2));
        KTEST_EXPECT_TRUE(loaded.used_bytes >= 64 + 96);
        KTEST_EXPECT_EQUAL(loaded.peak_used, loaded.used_bytes);
        KTEST_EXPECT_TRUE(loaded.largest_free <= loaded.free_bytes);

        g_test_heap.free(b);
        auto drained = g_test_heap.stats();
        KTEST_EXPECT_EQUAL(drained.free_calls, static_cast<uint64_t>(1));
        KTEST_EXPECT_TRUE(drained.used_bytes < loaded.used_bytes);
        KTEST_EXPECT_EQUAL(drained.peak_used, loaded.peak_used);
        g_test_heap.free(a);
    }

    // Phase 2: the block walk totals match the stats.
    reset_shared_heap(512);
    {
        void* a = g_test_heap.alloc(64, 16);
        KTEST_REQUIRE_TRUE(a != nullptr);

        struct walk_totals {
            size_t blocks    = 0;
            size_t used      = 0;
            size_t free_seen = 0;
        } totals;
        g_test_heap.for_each_block(
            [](void* ctx, size_t payload, bool is_free) {
                auto* t = static_cast<walk_totals*>(ctx);
                ++t->blocks;
                if (is_free) {
                    t->free_seen += payload;
                } else {
                    t->used += payload;
                }
            },
            &totals);

        auto s = g_test_heap.stats();
        KTEST_EXPECT_EQUAL(totals.blocks, s.blocks);
        KTEST_EXPECT_EQUAL(totals.used, s.used_bytes);
        KTEST_EXPECT_EQUAL(totals.free_seen, s.free_bytes);
        g_test_heap.free(a);
    }
}
