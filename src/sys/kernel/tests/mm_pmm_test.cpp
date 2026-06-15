#include <stddef.h>
#include <stdint.h>

#include "kernel/config.h"
#include "kernel/mm/page.h"
#include "kernel/mm/pmm.h"
#include "kernel/testing/testing.h"

// These tests drive the global page frame allocator, which zeroes pages through
// the real HHDM mapping and mutates the shared free/dirty pools. They are marked
// as integration tests so the harness reboots into a fresh VM before and after
// each, giving every test a pristine, deterministic allocator state.

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

KTEST_INTEGRATION(pmm_free_then_alloc_recycles_page, "mm/pmm") {
    auto first = kernel::mm::g_page_frame_allocator.alloc();
    KTEST_REQUIRE_TRUE(first.has_value());
    kernel::mm::g_page_frame_allocator.free(first.value());

    // With the zeroed pool empty, a freed page lands on the dirty stack and is
    // the next page handed back.
    auto second = kernel::mm::g_page_frame_allocator.alloc();
    KTEST_REQUIRE_TRUE(second.has_value());
    KTEST_EXPECT_EQUAL(second.value(), first.value());

    kernel::mm::g_page_frame_allocator.free(second.value());
}

KTEST_INTEGRATION(pmm_recycled_page_is_rezeroed, "mm/pmm") {
    auto page = kernel::mm::g_page_frame_allocator.alloc();
    KTEST_REQUIRE_TRUE(page.has_value());
    kernel::mm::vm_paddr_t addr = page.value();

    // Dirty the page, then free and reclaim it; the allocator must zero it again.
    auto* words                 = reinterpret_cast<volatile uint64_t*>(addr + g_hhdm_offset);
    for (size_t i = 0; i < PAGE_SIZE / sizeof(uint64_t); ++i) { words[i] = 0xDEADBEEFCAFEF00DULL; }

    kernel::mm::g_page_frame_allocator.free(addr);
    auto reclaimed = kernel::mm::g_page_frame_allocator.alloc();
    KTEST_REQUIRE_TRUE(reclaimed.has_value());
    KTEST_REQUIRE_EQUAL(reclaimed.value(), addr);
    KTEST_EXPECT_TRUE(page_is_zeroed(addr));

    kernel::mm::g_page_frame_allocator.free(addr);
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
