#include <stddef.h>
#include <stdint.h>

#include "kernel/arch.h"
#include "kernel/mm/page_descriptor.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/vm_aspace.h"
#include "kernel/testing/testing.h"

// These tests observe the global page descriptor array populated at VMM init
// from the real Limine memmap, and drive the shared PMM. They are merged into
// two integration stories, each against one freshly booted VM: the descriptor
// array's view of the boot memory map and of PMM transitions, and the kernel
// address space adopted from the boot page tables. Every fact asserted here is
// a standing invariant of a running kernel (the image stays wired, MMIO holes
// stay MMIO), so the phases can safely share the VM.

extern uintptr_t g_hhdm_offset;

KTEST_MODULE("mm/descriptor");

namespace { using namespace kernel::mm; }

// Story: the descriptor array reflects the boot memory map and tracks PMM
// state transitions.
KTEST_CASE_INTEGRATION(descriptor_array_tracks_boot_map_and_pmm) {
    // Phase 1: populated at boot, with the states the memmap implies.
    KTEST_REQUIRE_TRUE(g_page_descriptors.initialized());
    KTEST_EXPECT_NOT_EQUAL(g_page_descriptors.coverage_end(), static_cast<vm_paddr_t>(0));
    // A booted kernel always has pinned frames (its own image, the array).
    KTEST_EXPECT_TRUE(g_page_descriptors.count(page_state::WIRED) > 0);
    KTEST_EXPECT_TRUE(g_page_descriptors.count(page_state::FREE) > 0);
    // Address holes below RAM (legacy ranges on x86_64, the sub-2GiB gap on
    // riscv64) are MMIO, not WIRED -- they must stay out of usage accounting.
    KTEST_EXPECT_TRUE(g_page_descriptors.count(page_state::MMIO) > 0);

    // Phase 2: resolve this test's own code through the kernel aspace; the
    // backing frame lives in the Limine kernel range and must be pinned.
    {
        auto& aspace = kernel_aspace();
        KTEST_REQUIRE_TRUE(aspace.is_valid());

        auto paddr = aspace.walk(reinterpret_cast<uintptr_t>(&kernel_aspace));
        KTEST_REQUIRE_TRUE(paddr.has_value());

        const page_descriptor* desc = g_page_descriptors.lookup(paddr.value());
        KTEST_REQUIRE_TRUE(desc != nullptr);
        KTEST_EXPECT_TRUE(desc->state == page_state::WIRED);
    }

    // Phase 3: lookups past the covered range are rejected.
    KTEST_EXPECT_TRUE(g_page_descriptors.lookup(g_page_descriptors.coverage_end()) == nullptr);
    KTEST_EXPECT_TRUE(g_page_descriptors.lookup(~static_cast<vm_paddr_t>(0)) == nullptr);

    // Phase 4: a PMM alloc/free round-trip is mirrored in the descriptor.
    {
        KTEST_REQUIRE_VALUE(probe, g_page_frame_allocator.alloc());

        page_descriptor* desc = g_page_descriptors.lookup(probe);
        KTEST_REQUIRE_TRUE(desc != nullptr);
        KTEST_EXPECT_TRUE(desc->state == page_state::ACTIVE);
        KTEST_EXPECT_TRUE(desc->owner == nullptr);
        KTEST_EXPECT_EQUAL(desc->share_count, 0u);

        g_page_frame_allocator.free(probe);
        // The zeroer thread may relabel the freed page ZEROED at any point;
        // both states mean the frame is back in the PMM.
        KTEST_EXPECT_TRUE(desc->state == page_state::FREE || desc->state == page_state::ZEROED);
    }
}

// Story: the kernel address space was adopted from the boot tables correctly:
// the root table is PMM-owned, the bootloader's lower half is gone, and the
// HHDM mapping resolves through it.
KTEST_CASE_INTEGRATION(kernel_aspace_adopted_from_boot_tables) {
    // Phase 1: the live root table must be a PMM-allocated frame (descriptor
    // ACTIVE), not a bootloader table squatting in reclaimable memory the PMM
    // could hand out as free.
    {
        uintptr_t root              = kernel::arch::active_translation_root();
        const page_descriptor* desc = g_page_descriptors.lookup(root & ~static_cast<uintptr_t>(0xFFF));
        KTEST_REQUIRE_TRUE(desc != nullptr);
        KTEST_EXPECT_TRUE(desc->state == page_state::ACTIVE);

        // Only the kernel half was carried over: the bootloader's lower-half
        // identity map is gone, so low addresses now fault like any other.
        KTEST_EXPECT_FALSE(kernel_aspace().walk(0x100000).has_value());
    }

    // Phase 2: the HHDM mapping made by the bootloader must resolve through
    // the adopted aspace, offset preserved.
    {
        KTEST_REQUIRE_VALUE(probe, g_page_frame_allocator.alloc());

        auto paddr = kernel_aspace().walk(g_hhdm_offset + probe + 0x123);
        KTEST_REQUIRE_TRUE(paddr.has_value());
        KTEST_EXPECT_EQUAL(paddr.value(), probe + 0x123);

        g_page_frame_allocator.free(probe);
    }
}
