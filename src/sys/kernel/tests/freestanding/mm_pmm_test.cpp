#include <stddef.h>
#include <stdint.h>

#include "kernel/config.h"
#include "kernel/mm/page.h"
#include "kernel/mm/page_descriptor.h"
#include "kernel/mm/pmm.h"
#include "kernel/testing/testing.h"

// These tests drive the global page frame allocator, which zeroes pages through
// the real HHDM mapping and mutates the shared free/dirty pools. They are marked
// as integration tests so the harness reboots into a fresh VM before and after
// each, giving every test a pristine, deterministic allocator state. The zeroer
// thread runs alongside them, so no test may assume which pool a page comes
// from -- only end-state invariants (counts, zeroed contents) are asserted.

// Set during boot from the Limine HHDM response; lets us touch a physical page
// through its higher-half identity mapping.
extern uintptr_t g_hhdm_offset;

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

KTEST_INTEGRATION(pmm_alloc_returns_zeroed_aligned_page, "mm/pmm") {
    auto page = kernel::mm::g_page_frame_allocator.alloc();
    KTEST_REQUIRE_TRUE(page.has_value());

    kernel::mm::vm_paddr_t addr = page.value();
    KTEST_REQUIRE_NOT_EQUAL(addr, static_cast<size_t>(0));
    KTEST_EXPECT_ALIGNED(addr, PAGE_SIZE);
    KTEST_EXPECT_TRUE(page_is_zeroed(addr));

    kernel::mm::g_page_frame_allocator.free(addr);
}

KTEST_INTEGRATION(pmm_free_restores_free_page_count, "mm/pmm") {
    // The zeroer thread moves pages between pools but never changes the free
    // count, so the accounting must balance exactly around an alloc/free pair.
    size_t baseline = kernel::mm::g_page_frame_allocator.free_pages();

    auto page       = kernel::mm::g_page_frame_allocator.alloc();
    KTEST_REQUIRE_TRUE(page.has_value());
    KTEST_EXPECT_EQUAL(kernel::mm::g_page_frame_allocator.free_pages(), baseline - 1);

    kernel::mm::g_page_frame_allocator.free(page.value());
    KTEST_EXPECT_EQUAL(kernel::mm::g_page_frame_allocator.free_pages(), baseline);
}

KTEST_INTEGRATION(pmm_recycled_page_is_rezeroed, "mm/pmm") {
    auto page = kernel::mm::g_page_frame_allocator.alloc();
    KTEST_REQUIRE_TRUE(page.has_value());
    kernel::mm::vm_paddr_t addr = page.value();

    // Dirty the page and free it, then allocate until it comes back. It may
    // return via the inline alloc path or via the zeroer thread, but every
    // page handed out along the way must be zeroed. Recycled pages sit ahead
    // of pre-zeroed region tails in alloc order, so the bound only needs to
    // clear the (small) pool of pages recycled since boot.
    dirty_page(addr);
    kernel::mm::g_page_frame_allocator.free(addr);

    constexpr size_t MAX_ALLOCS = 256;
    kernel::mm::vm_paddr_t taken[MAX_ALLOCS];
    size_t taken_count = 0;
    bool found         = false;
    while (taken_count < MAX_ALLOCS) {
        auto p = kernel::mm::g_page_frame_allocator.alloc();
        KTEST_REQUIRE_TRUE(p.has_value());
        KTEST_EXPECT_TRUE(page_is_zeroed(p.value()));
        taken[taken_count++] = p.value();
        if (p.value() == addr) {
            found = true;
            break;
        }
    }
    KTEST_REQUIRE_TRUE(found);

    for (size_t i = 0; i < taken_count; ++i) { kernel::mm::g_page_frame_allocator.free(taken[i]); }
}

KTEST_INTEGRATION(pmm_consecutive_allocs_are_distinct, "mm/pmm") {
    auto a = kernel::mm::g_page_frame_allocator.alloc();
    auto b = kernel::mm::g_page_frame_allocator.alloc();
    auto c = kernel::mm::g_page_frame_allocator.alloc();
    KTEST_REQUIRE_TRUE(a.has_value());
    KTEST_REQUIRE_TRUE(b.has_value());
    KTEST_REQUIRE_TRUE(c.has_value());

    KTEST_EXPECT_NOT_EQUAL(a.value(), b.value());
    KTEST_EXPECT_NOT_EQUAL(b.value(), c.value());
    KTEST_EXPECT_NOT_EQUAL(a.value(), c.value());

    kernel::mm::g_page_frame_allocator.free(a.value());
    kernel::mm::g_page_frame_allocator.free(b.value());
    kernel::mm::g_page_frame_allocator.free(c.value());
}

KTEST_INTEGRATION(pmm_zeroer_prezeroes_half_of_free_memory, "mm/pmm") {
    // Drive the zeroer's work loop to quiescence; the background thread may do
    // any share of the work, both paths funnel through zero_one_page.
    while (kernel::mm::g_page_frame_allocator.zero_one_page()) {}
    KTEST_EXPECT_TRUE(kernel::mm::g_page_frame_allocator.zeroed_pages() >=
                      kernel::mm::g_page_frame_allocator.free_pages() / 2);

    // Page descriptor states must track the allocator's own accounting.
    KTEST_EXPECT_EQUAL(kernel::mm::g_page_descriptors.count(kernel::mm::page_state::ZEROED),
                       kernel::mm::g_page_frame_allocator.zeroed_pages());

    // A pre-zeroed page skips alloc's inline memset, so it must already be
    // clean.
    auto page = kernel::mm::g_page_frame_allocator.alloc();
    KTEST_REQUIRE_TRUE(page.has_value());
    KTEST_EXPECT_TRUE(page_is_zeroed(page.value()));
    kernel::mm::g_page_frame_allocator.free(page.value());
}

KTEST_INTEGRATION(pmm_dirty_page_repooled_zeroed, "mm/pmm") {
    // A dirtied, freed page drained through zero_one_page must come back from
    // the zeroed pool actually clean; alloc does not re-zero pool pages, so a
    // missed memset in zero_one_page shows up as a dirty allocation here.
    auto page = kernel::mm::g_page_frame_allocator.alloc();
    KTEST_REQUIRE_TRUE(page.has_value());
    kernel::mm::vm_paddr_t addr = page.value();
    dirty_page(addr);
    kernel::mm::g_page_frame_allocator.free(addr);

    // Stop once the page is clean rather than draining to full quiescence;
    // dirty pages drain ahead of region pre-zeroing, so this stays short.
    while (!page_is_zeroed(addr) && kernel::mm::g_page_frame_allocator.zero_one_page()) {}
    KTEST_EXPECT_TRUE(page_is_zeroed(addr));
}

KTEST_INTEGRATION(pmm_stats_track_alloc_free_and_low_water, "mm/pmm") {
    auto before = kernel::mm::g_page_frame_allocator.stats();
    KTEST_EXPECT_TRUE(before.free_pages + before.reserved_pages <= before.total_pages);
    // Zeroed pages are a subset of free pages within any one snapshot.
    KTEST_EXPECT_TRUE(before.zeroed_pooled + before.zeroed_region_tail <= before.free_pages);

    auto page = kernel::mm::g_page_frame_allocator.alloc();
    KTEST_REQUIRE_TRUE(page.has_value());
    auto during = kernel::mm::g_page_frame_allocator.stats();
    KTEST_EXPECT_EQUAL(during.alloc_count, before.alloc_count + 1);
    KTEST_EXPECT_TRUE(during.low_water <= during.free_pages);
    KTEST_EXPECT_TRUE(during.low_water <= before.free_pages - 1);

    kernel::mm::g_page_frame_allocator.free(page.value());
    auto after = kernel::mm::g_page_frame_allocator.stats();
    KTEST_EXPECT_EQUAL(after.free_count, before.free_count + 1);
    KTEST_EXPECT_EQUAL(after.free_pages, before.free_pages);
}
