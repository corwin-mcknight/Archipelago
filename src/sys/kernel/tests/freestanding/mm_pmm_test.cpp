#include <stddef.h>
#include <stdint.h>

#include "kernel/config.h"
#include "kernel/mm/page.h"
#include "kernel/mm/page_descriptor.h"
#include "kernel/mm/pmm.h"
#include "kernel/testing/testing.h"

// These tests drive the global page frame allocator, which zeroes pages through
// the real HHDM mapping and mutates the shared free/dirty pools. They are
// merged into two integration stories, each booting one fresh VM. Within a
// story the phases share that VM, so every phase snapshots the counters it
// needs at its own start and asserts deltas; only the story's first phase may
// rely on anything boot-shaped. The zeroer thread runs alongside them, so no
// phase may assume which pool a page comes from -- only end-state invariants
// (counts, zeroed contents) are asserted.

// Set during boot from the Limine HHDM response; lets us touch a physical page
// through its higher-half identity mapping.
extern uintptr_t g_hhdm_offset;

KTEST_MODULE("mm/pmm");

namespace {

constexpr size_t PAGE_SIZE = KERNEL_MINIMUM_PAGE_SIZE;

// Reads a freshly allocated page through the HHDM mapping and reports whether
// every byte is zero.
bool page_is_zeroed(kernel::mm::vm_paddr_t addr) {
    auto* words = reinterpret_cast<volatile uint64_t*>(addr + g_hhdm_offset);
    for (size_t i = 0; i < PAGE_SIZE / sizeof(uint64_t); ++i) {
        if (words[i] != 0) { return false; }
    }
    return true;
}

void dirty_page(kernel::mm::vm_paddr_t addr) {
    auto* words = reinterpret_cast<volatile uint64_t*>(addr + g_hhdm_offset);
    for (size_t i = 0; i < PAGE_SIZE / sizeof(uint64_t); ++i) { words[i] = 0xDEADBEEFCAFEF00DULL; }
}

}  // namespace

// Story: the single-page allocation lifecycle. A fresh allocation is zeroed
// and aligned, an alloc/free pair balances the free-page count exactly,
// consecutive allocations are distinct, and a dirtied-then-freed page is
// re-zeroed before it is ever handed out again.
KTEST_CASE_INTEGRATION(pmm_allocation_lifecycle) {
    auto& pmm = kernel::mm::g_page_frame_allocator;

    // Phase 1: a fresh allocation is a nonzero, page-aligned, zeroed frame.
    {
        KTEST_REQUIRE_VALUE(addr, pmm.alloc());
        KTEST_REQUIRE_NOT_EQUAL(addr, static_cast<size_t>(0));
        KTEST_EXPECT_ALIGNED(addr, PAGE_SIZE);
        KTEST_EXPECT_TRUE(page_is_zeroed(addr));
        pmm.free(addr);
    }

    // Phase 2: the zeroer thread moves pages between pools but never changes
    // the free count, so the accounting must balance exactly around an
    // alloc/free pair (baseline snapshotted here, not at boot).
    {
        size_t baseline = pmm.free_pages();
        KTEST_REQUIRE_VALUE(addr, pmm.alloc());
        KTEST_EXPECT_EQUAL(pmm.free_pages(), baseline - 1);
        pmm.free(addr);
        KTEST_EXPECT_EQUAL(pmm.free_pages(), baseline);
    }

    // Phase 3: consecutive allocations are distinct frames.
    {
        KTEST_REQUIRE_VALUE(a, pmm.alloc());
        KTEST_REQUIRE_VALUE(b, pmm.alloc());
        KTEST_REQUIRE_VALUE(c, pmm.alloc());
        KTEST_EXPECT_ALL(a != b, b != c, a != c);
        pmm.free(a);
        pmm.free(b);
        pmm.free(c);
    }

    // Phase 4: dirty the page and free it, then allocate until it comes back.
    // It may return via the inline alloc path or via the zeroer thread, but
    // every page handed out along the way must be zeroed. Recycled pages sit
    // ahead of pre-zeroed region tails in alloc order, so the bound only needs
    // to clear the small pool of pages recycled since boot plus the handful
    // the earlier phases cycled through.
    {
        KTEST_REQUIRE_VALUE(addr, pmm.alloc());
        dirty_page(addr);
        pmm.free(addr);

        constexpr size_t MAX_ALLOCS = 256;
        kernel::mm::vm_paddr_t taken[MAX_ALLOCS];
        size_t taken_count = 0;
        bool found         = false;
        while (taken_count < MAX_ALLOCS) {
            auto p = pmm.alloc();
            KTEST_REQUIRE_TRUE(p.has_value());
            KTEST_EXPECT_TRUE(page_is_zeroed(p.value()));
            taken[taken_count++] = p.value();
            if (p.value() == addr) {
                found = true;
                break;
            }
        }
        KTEST_REQUIRE_TRUE(found);

        for (size_t i = 0; i < taken_count; ++i) { pmm.free(taken[i]); }
    }
}

// Story: the zeroer and the statistics counters. Draining the zeroer reaches
// the half-of-free-memory target with descriptors in agreement, a dirtied
// freed page drained through zero_one_page comes back actually clean, and the
// stats counters track alloc/free and the low-water mark.
KTEST_CASE_INTEGRATION(pmm_zeroer_and_stats) {
    auto& pmm = kernel::mm::g_page_frame_allocator;

    // Phase 1: drive the zeroer's work loop to quiescence; the background
    // thread may do any share of the work, both paths funnel through
    // zero_one_page.
    {
        while (pmm.zero_one_page()) {}
        KTEST_EXPECT_TRUE(pmm.zeroed_pages() >= pmm.free_pages() / 2);

        // Page descriptor states must track the allocator's own accounting.
        KTEST_EXPECT_EQUAL(kernel::mm::g_page_descriptors.count(kernel::mm::page_state::ZEROED), pmm.zeroed_pages());

        // A pre-zeroed page skips alloc's inline memset, so it must already be
        // clean.
        KTEST_REQUIRE_VALUE(page, pmm.alloc());
        KTEST_EXPECT_TRUE(page_is_zeroed(page));
        pmm.free(page);
    }

    // Phase 2: a dirtied, freed page drained through zero_one_page must come
    // back from the zeroed pool actually clean; alloc does not re-zero pool
    // pages, so a missed memset in zero_one_page shows up as a dirty
    // allocation here.
    {
        KTEST_REQUIRE_VALUE(addr, pmm.alloc());
        dirty_page(addr);
        pmm.free(addr);

        // Stop once the page is clean rather than draining to full quiescence;
        // dirty pages drain ahead of region pre-zeroing, so this stays short.
        while (!page_is_zeroed(addr) && pmm.zero_one_page()) {}
        KTEST_EXPECT_TRUE(page_is_zeroed(addr));
    }

    // Phase 3: stats track alloc/free counts and the low-water mark. All
    // comparisons are deltas against the snapshot taken here; low_water is
    // monotone since boot, so it is at most this phase's starting free count.
    {
        auto before = pmm.stats();
        KTEST_EXPECT_TRUE(before.free_pages + before.reserved_pages <= before.total_pages);
        // Zeroed pages are a subset of free pages within any one snapshot.
        KTEST_EXPECT_TRUE(before.zeroed_pooled + before.zeroed_region_tail <= before.free_pages);

        KTEST_REQUIRE_VALUE(page, pmm.alloc());
        auto during = pmm.stats();
        KTEST_EXPECT_EQUAL(during.alloc_count, before.alloc_count + 1);
        KTEST_EXPECT_TRUE(during.low_water <= during.free_pages);
        KTEST_EXPECT_TRUE(during.low_water <= before.free_pages - 1);

        pmm.free(page);
        auto after = pmm.stats();
        KTEST_EXPECT_EQUAL(after.free_count, before.free_count + 1);
        KTEST_EXPECT_EQUAL(after.free_pages, before.free_pages);
    }
}
