#include <stddef.h>
#include <stdint.h>

#include <ktl/ref>
#include <ktl/result>

#include "kernel/mm/page_descriptor.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/region.h"
#include "kernel/mm/vm_aspace.h"
#include "kernel/mm/vmo.h"
#include "kernel/testing/testing.h"

// Device-pager tests wrap a PMM-carved scratch range as an MMIO-style window
// and fault it into the kernel aspace. Integration-tier: fresh VM per test.

extern uintptr_t g_hhdm_offset;

namespace {
using namespace kernel::mm;

constexpr size_t PAGES       = 2;
constexpr size_t PAGE        = 0x1000;
constexpr uintptr_t MAP_BASE = 0x10000000000;  // above the 4 GiB boot identity map
constexpr vm_prot_t RW       = vm_prot::READ | vm_prot::WRITE;
}  // namespace

KTEST_INTEGRATION(device_vmo_maps_window_uncached, "mm/device") {
    // A wired scratch window standing in for MMIO.
    auto window = kernel::mm::g_page_frame_allocator.alloc_contiguous(PAGES);
    KTEST_REQUIRE_TRUE(window.has_value());
    vm_paddr_t phys = window.value();

    auto v          = create_device_vmo(phys, PAGES, vm_cache_mode::DEVICE);
    KTEST_REQUIRE_TRUE(v.get() != nullptr);

    // Window frames are pinned.
    page_descriptor* desc = g_page_descriptors.lookup(phys);
    KTEST_REQUIRE_TRUE(desc != nullptr);
    KTEST_EXPECT_TRUE(desc->state == page_state::WIRED);

    KTEST_REQUIRE_TRUE(kernel_aspace().root().map(MAP_BASE, PAGES * PAGE, v, 0, RW).is_ok());

    size_t free_before                                     = kernel::mm::g_page_frame_allocator.free_pages();

    // Touch both pages: fills translate, they must not allocate frames.
    *reinterpret_cast<volatile uint64_t*>(MAP_BASE)        = 0x11D0'11CE'0000'0001ull;
    *reinterpret_cast<volatile uint64_t*>(MAP_BASE + PAGE) = 0x11D0'11CE'0000'0002ull;
    KTEST_EXPECT_EQUAL(v->resident_pages(), PAGES);
    KTEST_EXPECT_EQUAL(v->fill_count(), static_cast<uint64_t>(PAGES));
    // No data frames were allocated: the delta is bookkeeping only -- one
    // residency chunk plus up to three intermediate page-table frames for a
    // previously untouched corner of the address space. Two more would mean
    // the pager allocated backing frames.
    KTEST_EXPECT_TRUE(free_before - kernel::mm::g_page_frame_allocator.free_pages() <= 4u);

    // Translations point straight into the window and carry the uncached
    // attribute end-to-end.
    auto t0 = kernel_aspace().arch().walk_ext(MAP_BASE);
    auto t1 = kernel_aspace().arch().walk_ext(MAP_BASE + PAGE);
    KTEST_REQUIRE_TRUE(t0.has_value() && t1.has_value());
    KTEST_EXPECT_EQUAL(t0.value().paddr, phys);
    KTEST_EXPECT_EQUAL(t1.value().paddr, phys + PAGE);
    KTEST_EXPECT_TRUE(t0.value().cache == vm_cache_mode::DEVICE);
    KTEST_EXPECT_TRUE(t1.value().cache == vm_cache_mode::DEVICE);

    // Writes went to the caller's physical range.
    KTEST_EXPECT_EQUAL(*reinterpret_cast<volatile uint64_t*>(phys + g_hhdm_offset), 0x11D0'11CE'0000'0001ull);
    KTEST_EXPECT_EQUAL(*reinterpret_cast<volatile uint64_t*>(phys + PAGE + g_hhdm_offset), 0x11D0'11CE'0000'0002ull);

    // Teardown: the binding unmaps; window frames stay wired and intact.
    KTEST_REQUIRE_TRUE(kernel_aspace().root().unmap(MAP_BASE, PAGES * PAGE).is_ok());
    KTEST_EXPECT_FALSE(kernel_aspace().arch().walk(MAP_BASE).has_value());
    v = ktl::ref<vmo>{};
    KTEST_EXPECT_TRUE(desc->state == page_state::WIRED);
    KTEST_EXPECT_EQUAL(*reinterpret_cast<volatile uint64_t*>(phys + g_hhdm_offset), 0x11D0'11CE'0000'0001ull);
}

KTEST_INTEGRATION(device_binding_degrades_cache_mode, "mm/device") {
    auto window = kernel::mm::g_page_frame_allocator.alloc_contiguous(1);
    KTEST_REQUIRE_TRUE(window.has_value());

    auto v = create_device_vmo(window.value(), 1, vm_cache_mode::DEVICE);
    KTEST_REQUIRE_TRUE(v.get() != nullptr);

    // Caller asked for CACHED; the pager's DEVICE mode wins (stricter-only).
    KTEST_REQUIRE_TRUE(kernel_aspace().root().map(MAP_BASE, PAGE, v, 0, RW, vm_cache_mode::CACHED).is_ok());
    *reinterpret_cast<volatile uint32_t*>(MAP_BASE) = 1;
    auto t                                          = kernel_aspace().arch().walk_ext(MAP_BASE);
    KTEST_REQUIRE_TRUE(t.has_value());
    KTEST_EXPECT_TRUE(t.value().cache == vm_cache_mode::DEVICE);

    KTEST_REQUIRE_TRUE(kernel_aspace().root().unmap(MAP_BASE, PAGE).is_ok());
}
