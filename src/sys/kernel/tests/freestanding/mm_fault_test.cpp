#include <stddef.h>
#include <stdint.h>

#include <ktl/ref>
#include <ktl/result>

#include "kernel/mm/page_descriptor.h"
#include "kernel/mm/region.h"
#include "kernel/mm/vm_aspace.h"
#include "kernel/mm/vmo.h"
#include "kernel/testing/testing.h"

// Demand-paging tests: real #PF faults resolved through the VMM in the kernel
// address space. Integration-tier -- each test faults against a fresh VM.

extern uintptr_t g_hhdm_offset;

namespace {
using namespace kernel::mm;

constexpr size_t PAGES       = 4;
constexpr size_t PAGE        = 0x1000;
// Above 4 GiB: the boot tables identity-map the first 4 GiB (Limine base
// revision 0), so lower addresses in the kernel aspace never fault.
constexpr uintptr_t MAP_BASE = 0x10000000000;  // 1 TiB
constexpr vm_prot_t RW       = vm_prot::READ | vm_prot::WRITE;
}  // namespace

KTEST_INTEGRATION(fault_read_maps_shared_zero_page, "mm/fault") {
    auto v = create_anonymous_vmo(PAGES);
    KTEST_REQUIRE_TRUE(v.get() != nullptr);
    KTEST_REQUIRE_TRUE(kernel_aspace().root().map(MAP_BASE, PAGES * PAGE, v, 0, RW).is_ok());

    uint64_t faults_before = kernel_aspace().fault_count();

    // First read demand-maps the global zero page read-only; no frame is
    // committed to the VMO.
    KTEST_EXPECT_EQUAL(*reinterpret_cast<volatile uint64_t*>(MAP_BASE), static_cast<uint64_t>(0));
    KTEST_EXPECT_EQUAL(*reinterpret_cast<volatile uint64_t*>(MAP_BASE + PAGE), static_cast<uint64_t>(0));
    KTEST_EXPECT_EQUAL(v->resident_pages(), 0u);
    KTEST_EXPECT_TRUE(kernel_aspace().fault_count() >= faults_before + 2);

    // Both pages share the one wired zero frame, mapped without WRITE.
    auto t0 = kernel_aspace().arch().walk_ext(MAP_BASE);
    auto t1 = kernel_aspace().arch().walk_ext(MAP_BASE + PAGE);
    KTEST_REQUIRE_TRUE(t0.has_value() && t1.has_value());
    KTEST_EXPECT_EQUAL(t0.value().paddr, vmm_zero_page());
    KTEST_EXPECT_EQUAL(t1.value().paddr, vmm_zero_page());
    KTEST_EXPECT_TRUE((t0.value().prot & vm_prot::WRITE) == 0);

    KTEST_REQUIRE_TRUE(kernel_aspace().root().unmap(MAP_BASE, PAGES * PAGE).is_ok());
}

KTEST_INTEGRATION(fault_write_breaks_cow_to_private_copy, "mm/fault") {
    auto v = create_anonymous_vmo(PAGES);
    KTEST_REQUIRE_TRUE(v.get() != nullptr);
    KTEST_REQUIRE_TRUE(kernel_aspace().root().map(MAP_BASE, PAGES * PAGE, v, 0, RW).is_ok());

    auto* word = reinterpret_cast<volatile uint64_t*>(MAP_BASE);

    // Read first so the page is the shared zero page, then write through it.
    KTEST_EXPECT_EQUAL(*word, static_cast<uint64_t>(0));
    *word = 0x5EED'F00D'CAFE'D00Dull;

    // The write landed in a private frame, not the zero page.
    KTEST_EXPECT_EQUAL(*word, 0x5EED'F00D'CAFE'D00Dull);
    KTEST_EXPECT_EQUAL(v->resident_pages(), 1u);
    auto t = kernel_aspace().arch().walk_ext(MAP_BASE);
    KTEST_REQUIRE_TRUE(t.has_value());
    KTEST_EXPECT_NOT_EQUAL(t.value().paddr, vmm_zero_page());
    KTEST_EXPECT_TRUE((t.value().prot & vm_prot::WRITE) != 0);

    // The zero page itself is untouched.
    KTEST_EXPECT_EQUAL(*reinterpret_cast<volatile uint64_t*>(vmm_zero_page() + g_hhdm_offset),
                       static_cast<uint64_t>(0));

    // The private frame is owned by the VMO and outlives the TLB: data
    // persists across a zap-and-refault.
    page_descriptor* desc = g_page_descriptors.lookup(t.value().paddr & ~(PAGE - 1));
    KTEST_REQUIRE_TRUE(desc != nullptr);
    KTEST_EXPECT_TRUE(desc->owner == v.get());
    KTEST_REQUIRE_TRUE(kernel_aspace().arch().unmap_page(MAP_BASE).has_value());
    KTEST_EXPECT_EQUAL(*word, 0x5EED'F00D'CAFE'D00Dull);

    KTEST_REQUIRE_TRUE(kernel_aspace().root().unmap(MAP_BASE, PAGES * PAGE).is_ok());
}

KTEST_INTEGRATION(fault_write_to_unpopulated_page_skips_zero_page, "mm/fault") {
    auto v = create_anonymous_vmo(PAGES);
    KTEST_REQUIRE_TRUE(v.get() != nullptr);
    KTEST_REQUIRE_TRUE(kernel_aspace().root().map(MAP_BASE, PAGES * PAGE, v, 0, RW).is_ok());

    // Writing an unread page fills a private frame directly.
    *reinterpret_cast<volatile uint32_t*>(MAP_BASE + 2 * PAGE) = 0xABAD1DEA;
    KTEST_EXPECT_EQUAL(*reinterpret_cast<volatile uint32_t*>(MAP_BASE + 2 * PAGE), 0xABAD1DEAu);
    KTEST_EXPECT_EQUAL(v->resident_pages(), 1u);
    KTEST_EXPECT_EQUAL(v->fill_count(), 1u);

    // The rest of that frame read back as zero (fresh frames are zeroed).
    KTEST_EXPECT_EQUAL(*reinterpret_cast<volatile uint32_t*>(MAP_BASE + 2 * PAGE + 8), 0u);

    KTEST_REQUIRE_TRUE(kernel_aspace().root().unmap(MAP_BASE, PAGES * PAGE).is_ok());
}

// A fault outside any binding must keep crash-dumping exactly as before the
// resolver existed (the harness inverts the outcome: crash = pass).
KTEST_CRASH_TEST(fault_outside_binding_still_crashes, "mm/fault") {
    volatile int* p = reinterpret_cast<int*>(0x20000000000);  // 2 TiB: canonical, unmapped, no binding
    *p              = 0;
}
