#include <kernel/mm/slab_heap.h>
#include <stddef.h>
#include <stdint.h>

#include <ktl/ref>
#include <ktl/result>

#include "kernel/mm/page_descriptor.h"
#include "kernel/mm/physmap.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/region.h"
#include "kernel/mm/vm_aspace.h"
#include "kernel/mm/vmo.h"
#include "kernel/testing/testing.h"

// Each integration case shares global PMM, heap and descriptor state across
// its phases. Counter checks use local baselines and warm the heap where
// needed so earlier phases do not affect the expected deltas.

KTEST_MODULE("mm/vmo");

namespace {
using namespace kernel::mm;

constexpr size_t PAGES       = 8;
constexpr uintptr_t MAP_BASE = 0x10000000;
constexpr vm_prot_t RW       = vm_prot::READ | vm_prot::WRITE;
}  // namespace

// Chunk-pointer arrays for these page counts exceed the heap's run limit.
// SIZE_MAX also covers overflow in additive chunk-count rounding. All three
// convenience factories must propagate the allocation failure.
KTEST_CASE(vmo_chunk_index_allocation_failure) {
    KTEST_REQUIRE_TRUE(heap_slab_active());
    auto window = g_page_frame_allocator.alloc();
    KTEST_REQUIRE_TRUE(window.has_value());
    auto* descriptor = g_page_descriptors.lookup(window.value());
    KTEST_REQUIRE_TRUE(descriptor != nullptr);
    KTEST_REQUIRE_TRUE(descriptor->state == page_state::ACTIVE);

    auto before = g_slab_heap.stats();
    {
        auto large   = create_anonymous_vmo(SIZE_MAX / KERNEL_MINIMUM_PAGE_SIZE);
        auto wrapped = create_anonymous_vmo(SIZE_MAX);
        auto wired   = create_wired_vmo(window.value(), SIZE_MAX);
        auto device  = create_device_vmo(window.value(), SIZE_MAX, vm_cache_mode::DEVICE);
        KTEST_EXPECT_FALSE(large);
        KTEST_EXPECT_FALSE(wrapped);
        KTEST_EXPECT_FALSE(wired);
        KTEST_EXPECT_FALSE(device);
    }
    auto after = g_slab_heap.stats();
    // All four requests must reach the allocator and release temporary pagers
    // and control blocks on failure.
    KTEST_EXPECT_EQUAL(after.failures, before.failures + 4);
    KTEST_EXPECT_EQUAL(after.large_allocs, before.large_allocs);
    KTEST_EXPECT_EQUAL(after.large_pages, before.large_pages);
    for (size_t i = 0; i < SLAB_HEAP_CLASS_COUNT; ++i) {
        KTEST_EXPECT_EQUAL(after.classes[i].live_objects, before.classes[i].live_objects);
    }
    // Failed device construction must leave the frame's descriptor state unchanged.
    KTEST_EXPECT_TRUE(descriptor->state == page_state::ACTIVE);
    g_page_frame_allocator.free(window.value());

    auto recovered = create_anonymous_vmo(1);
    KTEST_REQUIRE_TRUE(recovered);
    KTEST_EXPECT_TRUE(recovered->commit(0, 1).is_ok());
}

// Story: the residency lifecycle. Commit populates distinct zeroed owned
// frames, and destroying a populated VMO is PMM-neutral.
KTEST_CASE(vmo_residency_lifecycle) {
    // Phase 1: commit populates residency, re-commit is a no-op, and
    // out-of-range commits are rejected.
    {
        auto v = create_anonymous_vmo(PAGES);
        KTEST_REQUIRE_TRUE(v.get() != nullptr);
        KTEST_EXPECT_EQUAL(v->size_pages(), PAGES);
        KTEST_EXPECT_EQUAL(v->resident_pages(), 0u);
        KTEST_EXPECT_FALSE(v->resident_frame(0).has_value());

        KTEST_REQUIRE_TRUE(v->commit(2, 3).is_ok());
        KTEST_EXPECT_EQUAL(v->resident_pages(), 3u);
        KTEST_EXPECT_EQUAL(v->fill_count(), 3u);
        KTEST_EXPECT_FALSE(v->resident_frame(1).has_value());

        // Each committed page has a distinct zeroed frame, owned by this VMO.
        for (uint64_t page = 2; page < 5; ++page) {
            auto frame = v->resident_frame(page);
            KTEST_REQUIRE_TRUE(frame.has_value());
            KTEST_EXPECT_EQUAL(*reinterpret_cast<volatile uint64_t*>(
                                   kernel::mm::unsafe::direct_map_address(kernel::mm::physical_address(frame.value()))),
                               static_cast<uint64_t>(0));

            page_descriptor* desc = g_page_descriptors.lookup(frame.value());
            KTEST_REQUIRE_TRUE(desc != nullptr);
            KTEST_EXPECT_TRUE(desc->state == page_state::ACTIVE);
            KTEST_EXPECT_TRUE(desc->owner == v.get());
            KTEST_EXPECT_EQUAL(desc->offset, page);
        }
        KTEST_EXPECT_NOT_EQUAL(v->resident_frame(2).value(), v->resident_frame(3).value());

        // Re-committing resident pages is a no-op.
        KTEST_REQUIRE_TRUE(v->commit(2, 3).is_ok());
        KTEST_EXPECT_EQUAL(v->fill_count(), 3u);

        // Out-of-range commits are rejected.
        KTEST_EXPECT_TRUE(v->commit(PAGES, 1).is_err());
        KTEST_EXPECT_TRUE(v->commit(PAGES - 1, 2).is_err());
    }

    // Phase 2: destruction is PMM-neutral. Warm the heap so vector and
    // control-block arenas don't skew the count (the earlier phase mostly does
    // this already, but keep the phase self-contained).
    {
        {
            auto warmup = create_anonymous_vmo(PAGES);
            KTEST_REQUIRE_TRUE(warmup.get() != nullptr);
            KTEST_REQUIRE_TRUE(warmup->commit(0, PAGES).is_ok());
        }

        size_t free_before = kernel::mm::g_page_frame_allocator.free_pages();
        {
            auto v = create_anonymous_vmo(PAGES);
            KTEST_REQUIRE_TRUE(v.get() != nullptr);
            KTEST_REQUIRE_TRUE(v->commit(0, PAGES).is_ok());
            KTEST_EXPECT_TRUE(kernel::mm::g_page_frame_allocator.free_pages() < free_before);
        }
        // Frames and residency chunks all returned on destruction.
        KTEST_EXPECT_EQUAL(kernel::mm::g_page_frame_allocator.free_pages(), free_before);
    }

    // Phase 3: the final page is usable for exact and partial chunk sizes.
    // The two-page commits straddle chunk boundaries for 513 and 1025 pages.
    constexpr size_t SIZES[] = {512, 513, 1025};
    for (size_t pages : SIZES) {
        auto v = create_anonymous_vmo(pages);
        KTEST_REQUIRE_TRUE(v);
        KTEST_EXPECT_EQUAL(v->size_pages(), pages);
        KTEST_EXPECT_FALSE(v->resident_frame(pages - 1).has_value());
        KTEST_REQUIRE_TRUE(v->commit(pages - 2, 2).is_ok());
        KTEST_EXPECT_EQUAL(v->resident_pages(), 2u);
        KTEST_EXPECT_NOT_EQUAL(v->resident_frame(pages - 2).value(), v->resident_frame(pages - 1).value());
        KTEST_EXPECT_ERR(v->commit(pages, 1), ktl::errc::out_of_range);
    }
}

// Story: mapping back-refs. Bindings register with the VMO on map, deregister
// on unmap, and binding validation rejects bad offsets.
KTEST_CASE(vmo_mapping_backrefs) {
    vm_aspace aspace;
    KTEST_REQUIRE_TRUE(aspace.init());

    auto v = create_anonymous_vmo(PAGES);
    KTEST_REQUIRE_TRUE(v.get() != nullptr);
    KTEST_EXPECT_EQUAL(v->mapping_count(), 0u);

    KTEST_REQUIRE_TRUE(aspace.root().map(MAP_BASE, PAGES * 0x1000, v, 0, RW).is_ok());
    KTEST_EXPECT_EQUAL(v->mapping_count(), 1u);

    KTEST_REQUIRE_TRUE(aspace.root().unmap(MAP_BASE, PAGES * 0x1000).is_ok());
    KTEST_EXPECT_EQUAL(v->mapping_count(), 0u);

    // Binding validation: offset must be page-aligned and in range.
    KTEST_EXPECT_TRUE(aspace.root().map(MAP_BASE, 0x1000, v, 0x123, RW).is_err());
    KTEST_EXPECT_TRUE(aspace.root().map(MAP_BASE, 0x2000, v, (PAGES - 1) * 0x1000, RW).is_err());
}
