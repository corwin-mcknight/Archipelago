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

// VMO tests drive the shared PMM and page descriptors; integration-tier so
// each runs against a fresh VM.

extern uintptr_t g_hhdm_offset;

namespace {
using namespace kernel::mm;

constexpr size_t PAGES       = 8;
constexpr uintptr_t MAP_BASE = 0x10000000;
constexpr vm_prot_t RW       = vm_prot::READ | vm_prot::WRITE;
}  // namespace

KTEST_INTEGRATION(vmo_commit_populates_residency, "mm/vmo") {
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
        KTEST_EXPECT_EQUAL(*reinterpret_cast<volatile uint64_t*>(frame.value() + g_hhdm_offset),
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

KTEST_INTEGRATION(vmo_decommit_releases_frames, "mm/vmo") {
    auto v = create_anonymous_vmo(PAGES);
    KTEST_REQUIRE_TRUE(v.get() != nullptr);
    KTEST_REQUIRE_TRUE(v->commit(0, PAGES).is_ok());

    auto frame = v->resident_frame(3);
    KTEST_REQUIRE_TRUE(frame.has_value());

    KTEST_REQUIRE_TRUE(v->decommit(3, 1).is_ok());
    KTEST_EXPECT_EQUAL(v->resident_pages(), PAGES - 1);
    KTEST_EXPECT_FALSE(v->resident_frame(3).has_value());

    // The released frame went back to the PMM with ownership cleared.
    page_descriptor* desc = g_page_descriptors.lookup(frame.value());
    KTEST_REQUIRE_TRUE(desc != nullptr);
    KTEST_EXPECT_TRUE(desc->state == page_state::FREE);
    KTEST_EXPECT_TRUE(desc->owner == nullptr);
}

KTEST_INTEGRATION(vmo_destruction_is_pmm_neutral, "mm/vmo") {
    // Warm the heap so vector/control-block arenas don't skew the count.
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

KTEST_INTEGRATION(vmo_mapping_backrefs_track_bindings, "mm/vmo") {
    vm_aspace aspace;
    KTEST_REQUIRE_TRUE(aspace.init());

    auto v = create_anonymous_vmo(PAGES);
    KTEST_REQUIRE_TRUE(v.get() != nullptr);
    KTEST_EXPECT_EQUAL(v->mapping_count(), 0u);

    KTEST_REQUIRE_TRUE(aspace.root().map(MAP_BASE, PAGES * 0x1000, v, 0, RW).is_ok());
    KTEST_EXPECT_EQUAL(v->mapping_count(), 1u);

    // Decommit consults the back-refs: a live translation inside the binding
    // is zapped when its page is decommitted.
    KTEST_REQUIRE_TRUE(v->commit(0, 1).is_ok());
    auto frame = v->resident_frame(0);
    KTEST_REQUIRE_TRUE(frame.has_value());
    KTEST_REQUIRE_TRUE(aspace.arch().map_page(MAP_BASE, frame.value(), RW));
    KTEST_REQUIRE_TRUE(v->decommit(0, 1).is_ok());
    KTEST_EXPECT_FALSE(aspace.arch().walk(MAP_BASE).has_value());

    KTEST_REQUIRE_TRUE(aspace.root().unmap(MAP_BASE, PAGES * 0x1000).is_ok());
    KTEST_EXPECT_EQUAL(v->mapping_count(), 0u);

    // Binding validation: offset must be page-aligned and in range.
    KTEST_EXPECT_TRUE(aspace.root().map(MAP_BASE, 0x1000, v, 0x123, RW).is_err());
    KTEST_EXPECT_TRUE(aspace.root().map(MAP_BASE, 0x2000, v, (PAGES - 1) * 0x1000, RW).is_err());
}

KTEST_INTEGRATION(vmo_resize_grow_and_shrink, "mm/vmo") {
    // Growth: mapped in the kernel aspace so real faults fill the new pages.
    // Above 4 GiB -- the boot identity map covers lower addresses.
    constexpr uintptr_t FAULT_BASE = 0x18000000000;
    auto v                         = create_anonymous_vmo(2);
    KTEST_REQUIRE_TRUE(v.get() != nullptr);
    KTEST_REQUIRE_TRUE(v->set_size(PAGES).is_ok());
    KTEST_EXPECT_EQUAL(v->size_pages(), PAGES);

    KTEST_REQUIRE_TRUE(kernel_aspace().root().map(FAULT_BASE, PAGES * 0x1000, v, 0, RW).is_ok());
    auto* grown_word = reinterpret_cast<volatile uint64_t*>(FAULT_BASE + (PAGES - 1) * 0x1000);
    *grown_word      = 0xC0FFEE;  // faults into a grown page
    KTEST_EXPECT_EQUAL(*grown_word, static_cast<uint64_t>(0xC0FFEE));
    KTEST_EXPECT_EQUAL(v->resident_pages(), 1u);

    // Shrink below the populated page: translation zapped, frame returned.
    size_t free_before = kernel::mm::g_page_frame_allocator.free_pages();
    KTEST_REQUIRE_TRUE(v->set_size(2).is_ok());
    KTEST_EXPECT_EQUAL(v->resident_pages(), 0u);
    KTEST_EXPECT_FALSE(kernel_aspace().arch().walk(FAULT_BASE + (PAGES - 1) * 0x1000).has_value());
    KTEST_EXPECT_EQUAL(kernel::mm::g_page_frame_allocator.free_pages(), free_before + 1);

    KTEST_REQUIRE_TRUE(kernel_aspace().root().unmap(FAULT_BASE, PAGES * 0x1000).is_ok());
}

KTEST_INTEGRATION(vmo_resize_rejected_for_device, "mm/vmo") {
    auto window = kernel::mm::g_page_frame_allocator.alloc_contiguous(1);
    KTEST_REQUIRE_TRUE(window.has_value());
    auto v = create_device_vmo(window.value(), 1, vm_cache_mode::DEVICE);
    KTEST_REQUIRE_TRUE(v.get() != nullptr);

    auto r = v->set_size(2);
    KTEST_REQUIRE_TRUE(r.is_err());
    KTEST_EXPECT_TRUE(r.unwrap_err() == ktl::errc::invalid_operation);
}

// After a shrink, a stale access through a still-mapped binding must be
// classified out-of-range and crash-dump (harness inverts: crash = pass).
KTEST_CRASH_TEST(vmo_access_past_shrunk_size_crashes, "mm/vmo") {
    constexpr uintptr_t FAULT_BASE = 0x1C000000000;
    auto v                         = create_anonymous_vmo(PAGES);
    if (v.get() == nullptr) { return; }  // clean exit = fail under inversion
    if (kernel_aspace().root().map(FAULT_BASE, PAGES * 0x1000, v, 0, RW).is_err()) { return; }

    // Populate the tail page via a zero-page read, then cut it off.
    (void)*reinterpret_cast<volatile uint64_t*>(FAULT_BASE + (PAGES - 1) * 0x1000);
    if (v->set_size(1).is_err()) { return; }

    *reinterpret_cast<volatile uint64_t*>(FAULT_BASE + (PAGES - 1) * 0x1000) = 1;  // out-of-range: must crash
}
