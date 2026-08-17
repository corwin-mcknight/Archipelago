#include <stddef.h>
#include <stdint.h>

#include <ktl/ref>
#include <ktl/result>

#include "kernel/mm/pmm.h"
#include "kernel/mm/region.h"
#include "kernel/mm/vm_aspace.h"
#include "kernel/testing/testing.h"

// Region-tree tests drive scratch address spaces (PMM-backed page tables).
// They are merged into two integration stories, each against one fresh VM:
// building and validating the tree, and mutating it (unmap, protect,
// teardown). Every phase constructs its own scratch aspace, so no phase
// depends on state left by another.

KTEST_MODULE("mm/region");

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

// Story: building the tree. A three-level hierarchy resolves deepest-binding
// lookups, and structurally invalid children and bindings are rejected.
KTEST_CASE_INTEGRATION(region_tree_construction_and_validation) {
    // Phase 1: three-level tree, deepest-binding lookup from the root.
    {
        vm_aspace aspace;
        KTEST_REQUIRE_TRUE(aspace.init());

        KTEST_UNWRAP(a, aspace.root().create_child(A_BASE, A_SIZE, RWX));
        KTEST_UNWRAP(b, a->create_child(B_BASE, B_SIZE, RW));
        KTEST_REQUIRE_TRUE(b->map(MAP_BASE, MAP_SIZE, ktl::ref<vmo>{}, 0, RW).is_ok());

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

    // Phase 2: invalid children and bindings are rejected.
    {
        vm_aspace aspace;
        KTEST_REQUIRE_TRUE(aspace.init());

        KTEST_UNWRAP(a_ref, aspace.root().create_child(A_BASE, A_SIZE, RW));

        // Out of parent bounds.
        KTEST_EXPECT_ERR(a_ref->create_child(A_BASE + A_SIZE, 0x1000, RW), ktl::errc::out_of_range);

        // Sibling overlap.
        KTEST_REQUIRE_TRUE(a_ref->create_child(B_BASE, B_SIZE, RW).is_ok());
        KTEST_EXPECT_ERR(a_ref->create_child(B_BASE + B_SIZE - 0x1000, 0x2000, RW), ktl::errc::invalid_operation);

        // Prot escalation past the parent's max-prot.
        KTEST_EXPECT_ERR(a_ref->create_child(A_BASE, 0x1000, RWX), ktl::errc::rights_violation);

        // Same checks apply to bindings.
        KTEST_EXPECT_ERR(a_ref->map(A_BASE, 0x1000, ktl::ref<vmo>{}, 0, RWX), ktl::errc::rights_violation);
    }
}

// Story: mutating the tree. Unmap and protect zap live translations under
// their bindings, partial cuts are rejected, and tearing down a whole child
// region cleans up its nested descendants.
KTEST_CASE_INTEGRATION(region_unmap_protect_and_teardown) {
    // Phase 1: unmap zaps translations; partial unmaps are rejected untouched.
    {
        vm_aspace aspace;
        KTEST_REQUIRE_TRUE(aspace.init());
        KTEST_REQUIRE_TRUE(aspace.root().map(MAP_BASE, MAP_SIZE, ktl::ref<vmo>{}, 0, RW).is_ok());

        // Simulate a fault fill: install a live translation inside the binding.
        KTEST_REQUIRE_VALUE(frame, kernel::mm::g_page_frame_allocator.alloc());
        KTEST_REQUIRE_TRUE(aspace.map_page(MAP_BASE, frame, RW));
        KTEST_REQUIRE_TRUE(aspace.walk(MAP_BASE).has_value());

        // Partial unmap cutting through the binding is rejected untouched.
        KTEST_EXPECT_ERR(aspace.root().unmap(MAP_BASE, 0x1000), ktl::errc::invalid_operation);
        KTEST_EXPECT_TRUE(aspace.walk(MAP_BASE).has_value());

        // Whole-slot unmap removes the binding and its translations.
        KTEST_REQUIRE_TRUE(aspace.root().unmap(MAP_BASE, MAP_SIZE).is_ok());
        KTEST_EXPECT_FALSE(aspace.walk(MAP_BASE).has_value());
        KTEST_EXPECT_TRUE(aspace.root().find_binding(MAP_BASE) == nullptr);

        kernel::mm::g_page_frame_allocator.free(frame);
    }

    // Phase 2: protect narrows only.
    {
        vm_aspace aspace;
        KTEST_REQUIRE_TRUE(aspace.init());
        KTEST_REQUIRE_TRUE(aspace.root().map(MAP_BASE, MAP_SIZE, ktl::ref<vmo>{}, 0, RW).is_ok());

        KTEST_REQUIRE_VALUE(frame, kernel::mm::g_page_frame_allocator.alloc());
        KTEST_REQUIRE_TRUE(aspace.map_page(MAP_BASE, frame, RW));

        // Narrowing succeeds, updates the binding, and zaps translations so
        // the fault path refills them with the new protection.
        KTEST_REQUIRE_TRUE(aspace.root().protect(MAP_BASE, MAP_SIZE, vm_prot::READ).is_ok());
        KTEST_EXPECT_FALSE(aspace.walk(MAP_BASE).has_value());
        region_child* hit = aspace.root().find_binding(MAP_BASE);
        KTEST_REQUIRE_TRUE(hit != nullptr);
        KTEST_EXPECT_EQUAL(hit->prot, vm_prot::READ);

        // Widening back is a rights violation.
        KTEST_EXPECT_ERR(aspace.root().protect(MAP_BASE, MAP_SIZE, RW), ktl::errc::rights_violation);

        kernel::mm::g_page_frame_allocator.free(frame);
    }

    // Phase 3: teardown of a whole child region leaves the space clean.
    {
        vm_aspace aspace;
        KTEST_REQUIRE_TRUE(aspace.init());

        KTEST_UNWRAP(a, aspace.root().create_child(A_BASE, A_SIZE, RW));
        KTEST_REQUIRE_TRUE(a->map(MAP_BASE, MAP_SIZE, ktl::ref<vmo>{}, 0, RW).is_ok());

        KTEST_REQUIRE_VALUE(frame, kernel::mm::g_page_frame_allocator.alloc());
        KTEST_REQUIRE_TRUE(aspace.map_page(MAP_BASE, frame, RW));

        // Unmapping the whole child region tears down its descendants: the
        // nested binding's translation must be gone from the arch space.
        KTEST_REQUIRE_TRUE(aspace.root().unmap(A_BASE, A_SIZE).is_ok());
        KTEST_EXPECT_FALSE(aspace.walk(MAP_BASE).has_value());

        kernel::mm::g_page_frame_allocator.free(frame);
    }
}

// Story: kernel-placed bindings. First-fit lands in the lowest gap at or
// above the floor under the same lock hold that inserts, and unmap-by-address
// removes exactly the direct-child binding containing the address.
KTEST_CASE_INTEGRATION(region_first_fit_and_unmap_binding) {
    // Phase 1: first-fit walks gaps in address order inside a bounded child
    // region, where exhaustion is reachable.
    {
        vm_aspace aspace;
        KTEST_REQUIRE_TRUE(aspace.init());
        KTEST_UNWRAP(a, aspace.root().create_child(A_BASE, 4 * 0x1000, RW));

        // Empty region: the floor itself wins; a floor below the region base
        // clamps up to it.
        KTEST_UNWRAP(first, a->map_anywhere(A_BASE + 0x1000, 0x1000, ktl::ref<vmo>{}, 0, RW));
        KTEST_EXPECT_EQUAL(first, A_BASE + 0x1000);
        KTEST_UNWRAP(second, a->map_anywhere(0, 0x1000, ktl::ref<vmo>{}, 0, RW));
        KTEST_EXPECT_EQUAL(second, A_BASE);

        // [base, base+2 pages) is now solid; the next fit is past it.
        KTEST_UNWRAP(third, a->map_anywhere(0, 0x2000, ktl::ref<vmo>{}, 0, RW));
        KTEST_EXPECT_EQUAL(third, A_BASE + 0x2000);

        // Full region: no gap anywhere, and no gap above a floor past the end.
        KTEST_EXPECT_ERR(a->map_anywhere(0, 0x1000, ktl::ref<vmo>{}, 0, RW), ktl::errc::capacity_exhausted);
        KTEST_EXPECT_ERR(a->map_anywhere(A_BASE + A_SIZE, 0x1000, ktl::ref<vmo>{}, 0, RW),
                         ktl::errc::capacity_exhausted);

        // A freed hole is found again, and an unaligned floor is rejected.
        KTEST_REQUIRE_TRUE(a->unmap_binding(A_BASE + 0x1000).is_ok());
        KTEST_UNWRAP(again, a->map_anywhere(0, 0x1000, ktl::ref<vmo>{}, 0, RW));
        KTEST_EXPECT_EQUAL(again, A_BASE + 0x1000);
        KTEST_EXPECT_ERR(a->map_anywhere(0x123, 0x1000, ktl::ref<vmo>{}, 0, RW), ktl::errc::invalid_operation);
    }

    // Phase 2: unmap_binding takes any interior address, zaps translations,
    // and refuses addresses naming nothing or naming a sub-region.
    {
        vm_aspace aspace;
        KTEST_REQUIRE_TRUE(aspace.init());
        KTEST_REQUIRE_TRUE(aspace.root().map(MAP_BASE, MAP_SIZE, ktl::ref<vmo>{}, 0, RW).is_ok());

        KTEST_REQUIRE_VALUE(frame, kernel::mm::g_page_frame_allocator.alloc());
        KTEST_REQUIRE_TRUE(aspace.map_page(MAP_BASE, frame, RW));

        // The last byte of the binding names it as well as the first.
        KTEST_REQUIRE_TRUE(aspace.root().unmap_binding(MAP_BASE + MAP_SIZE - 1).is_ok());
        KTEST_EXPECT_FALSE(aspace.walk(MAP_BASE).has_value());
        KTEST_EXPECT_TRUE(aspace.root().find_binding(MAP_BASE) == nullptr);

        // Nothing there anymore; one past a live binding is nothing too.
        KTEST_EXPECT_ERR(aspace.root().unmap_binding(MAP_BASE), ktl::errc::out_of_range);

        // A sub-region child is not a binding and is not removable this way.
        KTEST_REQUIRE_TRUE(aspace.root().create_child(A_BASE, A_SIZE, RW).is_ok());
        KTEST_EXPECT_ERR(aspace.root().unmap_binding(A_BASE), ktl::errc::out_of_range);

        kernel::mm::g_page_frame_allocator.free(frame);
    }
}
