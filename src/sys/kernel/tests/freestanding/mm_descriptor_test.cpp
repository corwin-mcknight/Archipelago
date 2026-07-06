#include <stddef.h>
#include <stdint.h>

#include "kernel/mm/page_descriptor.h"
#include "kernel/mm/paging.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/vm_aspace.h"
#include "kernel/testing/testing.h"

// These tests observe the global page descriptor array populated at VMM init
// from the real Limine memmap, and drive the shared PMM. Integration-tier so
// each runs against a freshly booted, deterministic VM.

extern uintptr_t g_hhdm_offset;

namespace { using namespace kernel::mm; }

KTEST_INTEGRATION(descriptor_array_initialized_at_boot, "mm/descriptor") {
    KTEST_REQUIRE_TRUE(g_page_descriptors.initialized());
    KTEST_EXPECT_NOT_EQUAL(g_page_descriptors.coverage_end(), static_cast<vm_paddr_t>(0));
    // A booted kernel always has pinned frames (its own image, the array).
    KTEST_EXPECT_TRUE(g_page_descriptors.count(page_state::WIRED) > 0);
    KTEST_EXPECT_TRUE(g_page_descriptors.count(page_state::FREE) > 0);
}

KTEST_INTEGRATION(descriptor_kernel_image_is_wired, "mm/descriptor") {
    // Resolve this test's own code through the kernel aspace; the backing
    // frame lives in the Limine kernel range and must be pinned.
    auto& aspace = kernel_aspace();
    KTEST_REQUIRE_TRUE(aspace.is_valid());

    auto paddr = aspace.walk(reinterpret_cast<uintptr_t>(&kernel_aspace));
    KTEST_REQUIRE_TRUE(paddr.has_value());

    const page_descriptor* desc = g_page_descriptors.lookup(paddr.value());
    KTEST_REQUIRE_TRUE(desc != nullptr);
    KTEST_EXPECT_TRUE(desc->state == page_state::WIRED);
}

KTEST_INTEGRATION(descriptor_lookup_rejects_uncovered, "mm/descriptor") {
    KTEST_EXPECT_TRUE(g_page_descriptors.lookup(g_page_descriptors.coverage_end()) == nullptr);
    KTEST_EXPECT_TRUE(g_page_descriptors.lookup(~static_cast<vm_paddr_t>(0)) == nullptr);
}

KTEST_INTEGRATION(descriptor_tracks_pmm_alloc_free, "mm/descriptor") {
    auto probe = g_page_frame_allocator.alloc();
    KTEST_REQUIRE_TRUE(probe.has_value());

    page_descriptor* desc = g_page_descriptors.lookup(probe.value());
    KTEST_REQUIRE_TRUE(desc != nullptr);
    KTEST_EXPECT_TRUE(desc->state == page_state::ACTIVE);
    KTEST_EXPECT_TRUE(desc->owner == nullptr);
    KTEST_EXPECT_EQUAL(desc->share_count, 0u);

    g_page_frame_allocator.free(probe.value());
    KTEST_EXPECT_TRUE(desc->state == page_state::FREE);
}

KTEST_INTEGRATION(kernel_aspace_walks_hhdm, "mm/descriptor") {
    // The kernel aspace adopted the boot tables, so the HHDM mapping made by
    // the bootloader must resolve through it, offset preserved.
    auto probe = g_page_frame_allocator.alloc();
    KTEST_REQUIRE_TRUE(probe.has_value());

    auto paddr = kernel_aspace().walk(g_hhdm_offset + probe.value() + 0x123);
    KTEST_REQUIRE_TRUE(paddr.has_value());
    KTEST_EXPECT_EQUAL(paddr.value(), probe.value() + 0x123);

    g_page_frame_allocator.free(probe.value());
}
