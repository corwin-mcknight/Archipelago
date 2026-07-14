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

// VMO tests drive the shared PMM and page descriptors. They are merged into
// three integration stories -- residency lifecycle, mapping back-refs, and
// resize -- each against one fresh VM, plus one crash test that takes the VM
// down. Phases inside a story snapshot the PMM counters they compare against
// at their own start, so they hold on a VM warmed by earlier phases.

extern uintptr_t g_hhdm_offset;

KTEST_MODULE("mm/vmo");

namespace {
using namespace kernel::mm;

constexpr size_t PAGES       = 8;
constexpr uintptr_t MAP_BASE = 0x10000000;
constexpr vm_prot_t RW       = vm_prot::READ | vm_prot::WRITE;
}  // namespace

// Story: the residency lifecycle. Commit populates distinct zeroed owned
// frames, decommit hands them back to the PMM with ownership cleared, and
// destroying a populated VMO is PMM-neutral.
KTEST_CASE_INTEGRATION(vmo_residency_lifecycle) {
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

    // Phase 2: decommit releases the frame back to the PMM.
    {
        auto v = create_anonymous_vmo(PAGES);
        KTEST_REQUIRE_TRUE(v.get() != nullptr);
        KTEST_REQUIRE_TRUE(v->commit(0, PAGES).is_ok());

        auto frame = v->resident_frame(3);
        KTEST_REQUIRE_TRUE(frame.has_value());

        KTEST_REQUIRE_TRUE(v->decommit(3, 1).is_ok());
        KTEST_EXPECT_EQUAL(v->resident_pages(), PAGES - 1);
        KTEST_EXPECT_FALSE(v->resident_frame(3).has_value());

        // The released frame went back to the PMM with ownership cleared. The
        // zeroer thread may relabel it ZEROED at any point; both states mean
        // the frame is back in the PMM.
        page_descriptor* desc = g_page_descriptors.lookup(frame.value());
        KTEST_REQUIRE_TRUE(desc != nullptr);
        KTEST_EXPECT_TRUE(desc->state == page_state::FREE || desc->state == page_state::ZEROED);
        KTEST_EXPECT_TRUE(desc->owner == nullptr);
    }

    // Phase 3: destruction is PMM-neutral. Warm the heap so vector and
    // control-block arenas don't skew the count (the earlier phases mostly do
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
}

// Story: mapping back-refs. Bindings register with the VMO, decommit and
// shrink consult the back-refs to zap live translations, and they zap them in
// every aspace the VMO is mapped into, not just one.
KTEST_CASE_INTEGRATION(vmo_mapping_backrefs) {
    // Phase 1: a single binding is tracked, its translation is zapped on
    // decommit, and binding validation rejects bad offsets.
    {
        vm_aspace aspace;
        KTEST_REQUIRE_TRUE(aspace.init());

        auto v = create_anonymous_vmo(PAGES);
        KTEST_REQUIRE_TRUE(v.get() != nullptr);
        KTEST_EXPECT_EQUAL(v->mapping_count(), 0u);

        KTEST_REQUIRE_TRUE(aspace.root().map(MAP_BASE, PAGES * 0x1000, v, 0, RW).is_ok());
        KTEST_EXPECT_EQUAL(v->mapping_count(), 1u);

        // Decommit consults the back-refs: a live translation inside the
        // binding is zapped when its page is decommitted.
        KTEST_REQUIRE_TRUE(v->commit(0, 1).is_ok());
        auto frame = v->resident_frame(0);
        KTEST_REQUIRE_TRUE(frame.has_value());
        KTEST_REQUIRE_TRUE(aspace.map_page(MAP_BASE, frame.value(), RW));
        KTEST_REQUIRE_TRUE(v->decommit(0, 1).is_ok());
        KTEST_EXPECT_FALSE(aspace.walk(MAP_BASE).has_value());

        KTEST_REQUIRE_TRUE(aspace.root().unmap(MAP_BASE, PAGES * 0x1000).is_ok());
        KTEST_EXPECT_EQUAL(v->mapping_count(), 0u);

        // Binding validation: offset must be page-aligned and in range.
        KTEST_EXPECT_TRUE(aspace.root().map(MAP_BASE, 0x1000, v, 0x123, RW).is_err());
        KTEST_EXPECT_TRUE(aspace.root().map(MAP_BASE, 0x2000, v, (PAGES - 1) * 0x1000, RW).is_err());
    }

    // Phase 2: one VMO mapped into two aspaces -- decommit and shrink walk the
    // mapping back-refs and zap the translation in every space, not just one.
    {
        vm_aspace a, b;
        KTEST_REQUIRE_TRUE(a.init());
        KTEST_REQUIRE_TRUE(b.init());
        auto v = create_anonymous_vmo(PAGES);
        KTEST_REQUIRE_TRUE(v.get() != nullptr);

        constexpr uintptr_t B_BASE = MAP_BASE + 0x40000000;  // different vaddr in b
        KTEST_REQUIRE_TRUE(a.root().map(MAP_BASE, PAGES * 0x1000, v, 0, RW).is_ok());
        KTEST_REQUIRE_TRUE(b.root().map(B_BASE, PAGES * 0x1000, v, 0, RW).is_ok());
        KTEST_EXPECT_EQUAL(v->mapping_count(), 2u);

        // Simulate fault fills of page 0 in both spaces.
        KTEST_REQUIRE_TRUE(v->commit(0, 1).is_ok());
        auto frame = v->resident_frame(0);
        KTEST_REQUIRE_TRUE(frame.has_value());
        KTEST_REQUIRE_TRUE(a.map_page(MAP_BASE, frame.value(), RW));
        KTEST_REQUIRE_TRUE(b.map_page(B_BASE, frame.value(), RW));

        KTEST_REQUIRE_TRUE(v->decommit(0, 1).is_ok());
        KTEST_EXPECT_FALSE(a.walk(MAP_BASE).has_value());
        KTEST_EXPECT_FALSE(b.walk(B_BASE).has_value());

        // Shrink zaps the tail in every aspace and frees the frame.
        KTEST_REQUIRE_TRUE(v->commit(PAGES - 1, 1).is_ok());
        auto tail = v->resident_frame(PAGES - 1);
        KTEST_REQUIRE_TRUE(tail.has_value());
        uintptr_t a_tail = MAP_BASE + (PAGES - 1) * 0x1000;
        uintptr_t b_tail = B_BASE + (PAGES - 1) * 0x1000;
        KTEST_REQUIRE_TRUE(a.map_page(a_tail, tail.value(), RW));
        KTEST_REQUIRE_TRUE(b.map_page(b_tail, tail.value(), RW));
        KTEST_REQUIRE_TRUE(v->set_size(1).is_ok());
        KTEST_EXPECT_FALSE(a.walk(a_tail).has_value());
        KTEST_EXPECT_FALSE(b.walk(b_tail).has_value());
        KTEST_EXPECT_EQUAL(v->resident_pages(), 0u);

        KTEST_REQUIRE_TRUE(a.root().unmap(MAP_BASE, PAGES * 0x1000).is_ok());
        KTEST_REQUIRE_TRUE(b.root().unmap(B_BASE, PAGES * 0x1000).is_ok());
        KTEST_EXPECT_EQUAL(v->mapping_count(), 0u);
    }
}

// Story: resizing. Growth exposes new pages to real faults, shrink zaps
// translations and returns frames, and device VMOs cannot be resized at all.
KTEST_CASE_INTEGRATION(vmo_resize) {
    // Phase 1: grow, fault into a grown page, then shrink below it.
    {
        // Growth: mapped in the kernel aspace so real faults fill the new
        // pages. Above 4 GiB -- the boot identity map covers lower addresses.
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
        KTEST_EXPECT_FALSE(kernel_aspace().walk(FAULT_BASE + (PAGES - 1) * 0x1000).has_value());
        KTEST_EXPECT_EQUAL(kernel::mm::g_page_frame_allocator.free_pages(), free_before + 1);

        KTEST_REQUIRE_TRUE(kernel_aspace().root().unmap(FAULT_BASE, PAGES * 0x1000).is_ok());
    }

    // Phase 2: resize is rejected for device VMOs.
    {
        auto window = kernel::mm::g_page_frame_allocator.alloc_contiguous(1);
        KTEST_REQUIRE_TRUE(window.has_value());
        auto v = create_device_vmo(window.value(), 1, vm_cache_mode::DEVICE);
        KTEST_REQUIRE_TRUE(v.get() != nullptr);

        auto r = v->set_size(2);
        KTEST_REQUIRE_TRUE(r.is_err());
        KTEST_EXPECT_TRUE(r.unwrap_err() == ktl::errc::invalid_operation);
    }
}

// After a shrink, a stale access through a still-mapped binding must be
// classified out-of-range and crash-dump (harness inverts: crash = pass).
KTEST_CASE_CRASH(vmo_access_past_shrunk_size_crashes) {
    constexpr uintptr_t FAULT_BASE = 0x1C000000000;
    auto v                         = create_anonymous_vmo(PAGES);
    if (v.get() == nullptr) { return; }  // clean exit = fail under inversion
    if (kernel_aspace().root().map(FAULT_BASE, PAGES * 0x1000, v, 0, RW).is_err()) { return; }

    // Populate the tail page via a zero-page read, then cut it off.
    (void)*reinterpret_cast<volatile uint64_t*>(FAULT_BASE + (PAGES - 1) * 0x1000);
    if (v->set_size(1).is_err()) { return; }

    *reinterpret_cast<volatile uint64_t*>(FAULT_BASE + (PAGES - 1) * 0x1000) = 1;  // out-of-range: must crash
}
