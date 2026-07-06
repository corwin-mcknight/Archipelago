#include <stddef.h>
#include <stdint.h>

#include <ktl/ref>
#include <ktl/result>

#include "kernel/mm/pmm.h"
#include "kernel/mm/region.h"
#include "kernel/mm/vm_aspace.h"
#include "kernel/testing/testing.h"

// Region-tree tests drive scratch address spaces (PMM-backed page tables), so
// they run integration-tier against a fresh VM.

namespace {
using namespace kernel::mm;

constexpr uintptr_t MB       = 1024 * 1024;
constexpr uintptr_t A_BASE   = 0x10000000;  // level-1 child region
constexpr size_t A_SIZE      = 16 * MB;
constexpr uintptr_t B_BASE   = A_BASE + MB;  // level-2 child region
constexpr size_t B_SIZE      = 2 * MB;
constexpr uintptr_t MAP_BASE = B_BASE + 0x1000;  // level-3 binding
constexpr size_t MAP_SIZE    = 0x2000;

constexpr vm_prot_t RW       = vm_prot::READ | vm_prot::WRITE;
constexpr vm_prot_t RWX      = RW | vm_prot::EXECUTE;
}  // namespace

KTEST_INTEGRATION(region_three_level_tree_and_lookup, "mm/region") {
    vm_aspace aspace;
    KTEST_REQUIRE_TRUE(aspace.init());

    auto a = aspace.root().create_child(A_BASE, A_SIZE, RWX);
    KTEST_REQUIRE_TRUE(a.is_ok());
    auto b = a.unwrap()->create_child(B_BASE, B_SIZE, RW);
    KTEST_REQUIRE_TRUE(b.is_ok());
    KTEST_REQUIRE_TRUE(b.unwrap()->map(MAP_BASE, MAP_SIZE, ktl::ref<vmo>{}, 0, RW).is_ok());

    // Deepest-binding lookup from the root descends both region levels.
    region_child* hit = aspace.root().find_binding(MAP_BASE + 0x1234);
    KTEST_REQUIRE_TRUE(hit != nullptr);
    KTEST_EXPECT_EQUAL(hit->base, MAP_BASE);
    KTEST_EXPECT_EQUAL(hit->size, MAP_SIZE);
    KTEST_EXPECT_TRUE(hit->is_binding());

    // Outside the binding but inside the regions: no hit.
    KTEST_EXPECT_TRUE(aspace.root().find_binding(MAP_BASE + MAP_SIZE) == nullptr);
    KTEST_EXPECT_TRUE(aspace.root().find_binding(A_BASE) == nullptr);
}

KTEST_INTEGRATION(region_rejects_invalid_children, "mm/region") {
    vm_aspace aspace;
    KTEST_REQUIRE_TRUE(aspace.init());

    auto a = aspace.root().create_child(A_BASE, A_SIZE, RW);
    KTEST_REQUIRE_TRUE(a.is_ok());
    auto a_ref = a.unwrap();  // unwrap() is move-out; bind once

    // Out of parent bounds.
    auto oob   = a_ref->create_child(A_BASE + A_SIZE, 0x1000, RW);
    KTEST_REQUIRE_TRUE(oob.is_err());
    KTEST_EXPECT_TRUE(oob.unwrap_err() == ktl::errc::out_of_range);

    // Sibling overlap.
    KTEST_REQUIRE_TRUE(a_ref->create_child(B_BASE, B_SIZE, RW).is_ok());
    auto overlap = a_ref->create_child(B_BASE + B_SIZE - 0x1000, 0x2000, RW);
    KTEST_REQUIRE_TRUE(overlap.is_err());
    KTEST_EXPECT_TRUE(overlap.unwrap_err() == ktl::errc::invalid_operation);

    // Prot escalation past the parent's max-prot.
    auto escalate = a_ref->create_child(A_BASE, 0x1000, RWX);
    KTEST_REQUIRE_TRUE(escalate.is_err());
    KTEST_EXPECT_TRUE(escalate.unwrap_err() == ktl::errc::rights_violation);

    // Same checks apply to bindings.
    auto bind_escalate = a_ref->map(A_BASE, 0x1000, ktl::ref<vmo>{}, 0, RWX);
    KTEST_REQUIRE_TRUE(bind_escalate.is_err());
    KTEST_EXPECT_TRUE(bind_escalate.unwrap_err() == ktl::errc::rights_violation);
}

KTEST_INTEGRATION(region_unmap_zaps_translations, "mm/region") {
    vm_aspace aspace;
    KTEST_REQUIRE_TRUE(aspace.init());
    KTEST_REQUIRE_TRUE(aspace.root().map(MAP_BASE, MAP_SIZE, ktl::ref<vmo>{}, 0, RW).is_ok());

    // Simulate a fault fill: install a live translation inside the binding.
    auto frame = kernel::mm::g_page_frame_allocator.alloc();
    KTEST_REQUIRE_TRUE(frame.has_value());
    KTEST_REQUIRE_TRUE(aspace.arch().map_page(MAP_BASE, frame.value(), RW));
    KTEST_REQUIRE_TRUE(aspace.arch().walk(MAP_BASE).has_value());

    // Partial unmap cutting through the binding is rejected untouched.
    auto partial = aspace.root().unmap(MAP_BASE, 0x1000);
    KTEST_REQUIRE_TRUE(partial.is_err());
    KTEST_EXPECT_TRUE(partial.unwrap_err() == ktl::errc::invalid_operation);
    KTEST_EXPECT_TRUE(aspace.arch().walk(MAP_BASE).has_value());

    // Whole-slot unmap removes the binding and its translations.
    KTEST_REQUIRE_TRUE(aspace.root().unmap(MAP_BASE, MAP_SIZE).is_ok());
    KTEST_EXPECT_FALSE(aspace.arch().walk(MAP_BASE).has_value());
    KTEST_EXPECT_TRUE(aspace.root().find_binding(MAP_BASE) == nullptr);

    kernel::mm::g_page_frame_allocator.free(frame.value());
}

KTEST_INTEGRATION(region_protect_narrows_only, "mm/region") {
    vm_aspace aspace;
    KTEST_REQUIRE_TRUE(aspace.init());
    KTEST_REQUIRE_TRUE(aspace.root().map(MAP_BASE, MAP_SIZE, ktl::ref<vmo>{}, 0, RW).is_ok());

    auto frame = kernel::mm::g_page_frame_allocator.alloc();
    KTEST_REQUIRE_TRUE(frame.has_value());
    KTEST_REQUIRE_TRUE(aspace.arch().map_page(MAP_BASE, frame.value(), RW));

    // Narrowing succeeds, updates the binding, and zaps translations so the
    // fault path refills them with the new protection.
    KTEST_REQUIRE_TRUE(aspace.root().protect(MAP_BASE, MAP_SIZE, vm_prot::READ).is_ok());
    KTEST_EXPECT_FALSE(aspace.arch().walk(MAP_BASE).has_value());
    region_child* hit = aspace.root().find_binding(MAP_BASE);
    KTEST_REQUIRE_TRUE(hit != nullptr);
    KTEST_EXPECT_EQUAL(hit->prot, vm_prot::READ);

    // Widening back is a rights violation.
    auto widen = aspace.root().protect(MAP_BASE, MAP_SIZE, RW);
    KTEST_REQUIRE_TRUE(widen.is_err());
    KTEST_EXPECT_TRUE(widen.unwrap_err() == ktl::errc::rights_violation);

    kernel::mm::g_page_frame_allocator.free(frame.value());
}

KTEST_INTEGRATION(region_teardown_leaves_space_clean, "mm/region") {
    vm_aspace aspace;
    KTEST_REQUIRE_TRUE(aspace.init());

    auto a = aspace.root().create_child(A_BASE, A_SIZE, RW);
    KTEST_REQUIRE_TRUE(a.is_ok());
    KTEST_REQUIRE_TRUE(a.unwrap()->map(MAP_BASE, MAP_SIZE, ktl::ref<vmo>{}, 0, RW).is_ok());

    auto frame = kernel::mm::g_page_frame_allocator.alloc();
    KTEST_REQUIRE_TRUE(frame.has_value());
    KTEST_REQUIRE_TRUE(aspace.arch().map_page(MAP_BASE, frame.value(), RW));

    // Unmapping the whole child region tears down its descendants: the nested
    // binding's translation must be gone from the arch space.
    KTEST_REQUIRE_TRUE(aspace.root().unmap(A_BASE, A_SIZE).is_ok());
    KTEST_EXPECT_FALSE(aspace.arch().walk(MAP_BASE).has_value());

    kernel::mm::g_page_frame_allocator.free(frame.value());
}
