#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/mm/paging.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vm_aspace.h>

// HHDM base, set during boot from the Limine response. Its virtual range is a
// kernel-half mapping the bootloader installs (huge pages on QEMU), so it lets
// us exercise kernel-clone visibility and huge-intermediate collision.
extern uintptr_t g_hhdm_offset;

KTEST_MODULE("mm/paging");

namespace {

using kernel::mm::vm_aspace;
using kernel::mm::vm_cache_mode;
using kernel::mm::vm_paddr_t;
using kernel::mm::vm_prot_t;
namespace vm_prot              = kernel::mm::vm_prot;

constexpr uintptr_t low_vaddr  = 0x100000;       // 1 MiB -- user half, empty in a fresh space
constexpr uintptr_t mid_vaddr  = 0x40000000;     // 1 GiB -- different PDPT entry
constexpr uintptr_t high_vaddr = 0x10000000000;  // 1 TiB -- different PML4 entry

}  // namespace

// Aspace lifecycle: init yields a valid rooted space, a second init on the
// same space is rejected, and destroy tears it back down.
KTEST_CASE(paging_init_destroy_lifecycle) {
    vm_aspace space;
    KTEST_REQUIRE_TRUE(space.init());
    KTEST_EXPECT_TRUE(space.is_valid());
    KTEST_EXPECT_TRUE(space.has_root());
    KTEST_EXPECT_FALSE(space.init());  // double init is rejected

    space.destroy();
    KTEST_EXPECT_FALSE(space.is_valid());
    KTEST_EXPECT_FALSE(space.has_root());
}

// Walk semantics: an unmapped address resolves to nothing through both walk
// entry points, and a mapped one resolves to the frame with the page offset
// preserved.
KTEST_CASE(paging_walk_semantics) {
    vm_aspace space;
    KTEST_REQUIRE_TRUE(space.init());

    KTEST_EXPECT_FALSE(space.walk(low_vaddr).has_value());
    KTEST_EXPECT_FALSE(space.walk_ext(low_vaddr).has_value());

    KTEST_REQUIRE_VALUE(frame, kernel::mm::g_page_frame_allocator.alloc());
    KTEST_REQUIRE_TRUE(space.map_page(low_vaddr, frame, vm_prot::READ | vm_prot::WRITE));
    KTEST_EXPECT_VALUE(space.walk(low_vaddr + 0x123), frame + 0x123);

    KTEST_REQUIRE_TRUE(space.unmap_page(low_vaddr).has_value());
    kernel::mm::g_page_frame_allocator.free(frame);
}

// Map/walk/unmap round-trip once per protection combination, verifying both the
// physical resolution and that walk_ext recovers the exact prot the mapping was
// installed with. x86_64 has no read-enable bit, so READ is always implied.
// The DEVICE cache mode joins the same attribute-roundtrip story: it degrades
// to uncached, and walk_ext must report a non-cached attribute.
KTEST_CASE(paging_prot_and_cache_roundtrip) {
    const vm_prot_t combos[] = {
        vm_prot::READ,
        vm_prot::READ | vm_prot::WRITE,
        vm_prot::READ | vm_prot::EXECUTE,
        vm_prot::READ | vm_prot::WRITE | vm_prot::EXECUTE,
        vm_prot::READ | vm_prot::USER,
        vm_prot::READ | vm_prot::WRITE | vm_prot::USER,
        vm_prot::READ | vm_prot::EXECUTE | vm_prot::USER,
        vm_prot::READ | vm_prot::WRITE | vm_prot::EXECUTE | vm_prot::USER,
    };

    for (vm_prot_t prot : combos) {
        vm_aspace space;
        KTEST_REQUIRE_TRUE(space.init());
        KTEST_REQUIRE_VALUE(frame, kernel::mm::g_page_frame_allocator.alloc());

        KTEST_REQUIRE_TRUE(space.map_page(low_vaddr, frame, prot));
        KTEST_EXPECT_VALUE(space.walk(low_vaddr), frame);

        auto ext = space.walk_ext(low_vaddr);
        KTEST_REQUIRE_TRUE(ext.has_value());
        KTEST_EXPECT_EQUAL(ext.value().paddr, frame);
        KTEST_EXPECT_EQUAL(ext.value().prot, prot);
        KTEST_EXPECT_TRUE(ext.value().cache == vm_cache_mode::CACHED);

        KTEST_EXPECT_VALUE(space.unmap_page(low_vaddr), frame);
        KTEST_EXPECT_FALSE(space.walk(low_vaddr).has_value());

        kernel::mm::g_page_frame_allocator.free(frame);
    }

    // DEVICE cache mode round-trips too.
    {
        vm_aspace space;
        KTEST_REQUIRE_TRUE(space.init());
        KTEST_REQUIRE_VALUE(frame, kernel::mm::g_page_frame_allocator.alloc());

        KTEST_REQUIRE_TRUE(space.map_page(low_vaddr, frame, vm_prot::READ | vm_prot::WRITE, vm_cache_mode::DEVICE));
        auto ext = space.walk_ext(low_vaddr);
        KTEST_REQUIRE_TRUE(ext.has_value());
        KTEST_EXPECT_TRUE(ext.value().cache == vm_cache_mode::DEVICE);

        KTEST_REQUIRE_TRUE(space.unmap_page(low_vaddr).has_value());
        kernel::mm::g_page_frame_allocator.free(frame);
    }
}

// The no-execute bit must round-trip: a mapping without EXECUTE reports no
// EXECUTE, one with EXECUTE reports it. Proves the old NO_EXECUTE rejection is
// gone and EFER.NXE is honored. (Regression pin -- kept separate.)
KTEST_CASE(paging_nx_is_honored) {
    vm_aspace space;
    KTEST_REQUIRE_TRUE(space.init());
    KTEST_REQUIRE_VALUE(frame, kernel::mm::g_page_frame_allocator.alloc());

    KTEST_REQUIRE_TRUE(space.map_page(low_vaddr, frame, vm_prot::READ | vm_prot::WRITE));
    auto no_exec = space.walk_ext(low_vaddr);
    KTEST_REQUIRE_TRUE(no_exec.has_value());
    KTEST_EXPECT_TRUE((no_exec.value().prot & vm_prot::EXECUTE) == 0);
    KTEST_REQUIRE_TRUE(space.unmap_page(low_vaddr).has_value());

    KTEST_REQUIRE_TRUE(space.map_page(low_vaddr, frame, vm_prot::READ | vm_prot::EXECUTE));
    auto exec = space.walk_ext(low_vaddr);
    KTEST_REQUIRE_TRUE(exec.has_value());
    KTEST_EXPECT_TRUE((exec.value().prot & vm_prot::EXECUTE) != 0);

    KTEST_REQUIRE_TRUE(space.unmap_page(low_vaddr).has_value());
    kernel::mm::g_page_frame_allocator.free(frame);
}

// Invalid map requests are rejected: an already-present leaf, unaligned
// virtual or physical addresses, and non-canonical addresses (bit 47 set but
// sign-extension broken) at every entry point. The bit-47 reasoning stays
// inside the arch layer.
KTEST_CASE(paging_invalid_map_rejected) {
    vm_aspace space;
    KTEST_REQUIRE_TRUE(space.init());
    KTEST_REQUIRE_VALUE(frame, kernel::mm::g_page_frame_allocator.alloc());

    // Double map of the same vaddr fails.
    KTEST_REQUIRE_TRUE(space.map_page(low_vaddr, frame, vm_prot::READ | vm_prot::WRITE));
    KTEST_EXPECT_FALSE(space.map_page(low_vaddr, frame, vm_prot::READ | vm_prot::WRITE));
    KTEST_REQUIRE_TRUE(space.unmap_page(low_vaddr).has_value());

    // Unaligned vaddr or paddr fails.
    KTEST_EXPECT_FALSE(space.map_page(low_vaddr + 1, frame, vm_prot::READ));
    KTEST_EXPECT_FALSE(space.map_page(low_vaddr, frame + 1, vm_prot::READ));

    // Non-canonical addresses are rejected by every entry point.
    constexpr uintptr_t non_canonical = 0x0000800000000000ull;
    KTEST_EXPECT_FALSE(space.map_page(non_canonical, frame, vm_prot::READ));
    KTEST_EXPECT_FALSE(space.walk(non_canonical).has_value());
    KTEST_EXPECT_FALSE(space.walk_ext(non_canonical).has_value());
    KTEST_EXPECT_FALSE(space.unmap_page(non_canonical).has_value());

    kernel::mm::g_page_frame_allocator.free(frame);
}

KTEST_CASE(paging_independent_mappings_across_levels) {
    vm_aspace space;
    KTEST_REQUIRE_TRUE(space.init());

    KTEST_REQUIRE_VALUE(p1, kernel::mm::g_page_frame_allocator.alloc());
    KTEST_REQUIRE_VALUE(p2, kernel::mm::g_page_frame_allocator.alloc());
    KTEST_REQUIRE_VALUE(p3, kernel::mm::g_page_frame_allocator.alloc());

    KTEST_REQUIRE_TRUE(space.map_page(low_vaddr, p1, vm_prot::READ | vm_prot::WRITE));
    KTEST_REQUIRE_TRUE(space.map_page(mid_vaddr, p2, vm_prot::READ | vm_prot::WRITE));
    KTEST_REQUIRE_TRUE(space.map_page(high_vaddr, p3, vm_prot::READ | vm_prot::WRITE));

    KTEST_EXPECT_VALUE(space.walk(low_vaddr), p1);
    KTEST_EXPECT_VALUE(space.walk(mid_vaddr), p2);
    KTEST_EXPECT_VALUE(space.walk(high_vaddr), p3);

    kernel::mm::g_page_frame_allocator.free(p1);
    kernel::mm::g_page_frame_allocator.free(p2);
    kernel::mm::g_page_frame_allocator.free(p3);
}

// init() clones the kernel half from the active space, so a fresh space can
// resolve a known kernel-half address (an HHDM offset) without any mapping of
// its own -- and mapping over that existing kernel-half mapping fails. On QEMU
// the HHDM is backed by huge pages, so the latter exercises the
// huge-intermediate collision path; if the bootloader used 4K pages instead it
// still fails as an already-present leaf.
KTEST_CASE(paging_kernel_half_cloned) {
    vm_aspace space;
    KTEST_REQUIRE_TRUE(space.init());

    uintptr_t kaddr = g_hhdm_offset + 0x1000;  // HHDM maps physical 0x1000 here
    KTEST_EXPECT_VALUE(space.walk(kaddr), static_cast<vm_paddr_t>(0x1000));

    KTEST_REQUIRE_VALUE(frame, kernel::mm::g_page_frame_allocator.alloc());
    uintptr_t hhdm_kaddr = (g_hhdm_offset + 0x200000) & ~static_cast<uintptr_t>(0xFFF);
    KTEST_EXPECT_FALSE(space.map_page(hhdm_kaddr, frame, vm_prot::READ | vm_prot::WRITE));

    kernel::mm::g_page_frame_allocator.free(frame);
}

// A low mapping in one space is invisible to another space: the user half is
// per-space, proving isolation.
KTEST_CASE(paging_user_half_is_isolated) {
    vm_aspace a, b;
    KTEST_REQUIRE_TRUE(a.init());
    KTEST_REQUIRE_TRUE(b.init());
    KTEST_REQUIRE_VALUE(frame, kernel::mm::g_page_frame_allocator.alloc());

    KTEST_REQUIRE_TRUE(b.map_page(low_vaddr, frame, vm_prot::READ | vm_prot::WRITE));
    KTEST_EXPECT_VALUE(b.walk(low_vaddr), frame);
    KTEST_EXPECT_FALSE(a.walk(low_vaddr).has_value());

    KTEST_REQUIRE_TRUE(b.unmap_page(low_vaddr).has_value());
    kernel::mm::g_page_frame_allocator.free(frame);
}

// Full activation round-trip: create a second space, map into it, activate it,
// touch the mapping through its virtual address, then reactivate the kernel space.
// Requires a clean VM because it switches the live address space.
KTEST_CASE_INTEGRATION(paging_activate_and_touch) {
    vm_aspace space;
    KTEST_REQUIRE_TRUE(space.init());
    KTEST_REQUIRE_VALUE(frame, kernel::mm::g_page_frame_allocator.alloc());

    // Supervisor mapping (no USER bit) so a CPL0 access is never blocked by SMAP.
    KTEST_REQUIRE_TRUE(space.map_page(mid_vaddr, frame, vm_prot::READ | vm_prot::WRITE));

    constexpr uint64_t magic = 0xA5A5C0FFEE00B00Dull;

    // Danger window: no aborting checks between activate() and the kernel-space restore.
    space.activate();
    auto* p       = reinterpret_cast<volatile uint64_t*>(mid_vaddr);
    *p            = magic;
    uint64_t seen = *p;
    kernel::mm::kernel_aspace().activate();

    KTEST_EXPECT_EQUAL(seen, magic);

    KTEST_REQUIRE_TRUE(space.unmap_page(mid_vaddr).has_value());
    kernel::mm::g_page_frame_allocator.free(frame);
}

#endif  // CONFIG_KERNEL_TESTING
