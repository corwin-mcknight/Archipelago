#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/mm/paging.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vm_aspace.h>

// HHDM base, set during boot from the Limine response. Its virtual range is a
// kernel-half mapping the bootloader installs (huge pages on QEMU), so it lets
// us exercise kernel-clone visibility and huge-intermediate collision.
extern uintptr_t g_hhdm_offset;

namespace {

using kernel::mm::vm_aspace;
using kernel::mm::vm_cache_mode;
using kernel::mm::vm_paddr_t;
using kernel::mm::vm_prot_t;
namespace vm_prot              = kernel::mm::vm_prot;

constexpr uintptr_t low_vaddr  = 0x100000;       // 1 MiB -- user half, empty in a fresh space
constexpr uintptr_t mid_vaddr  = 0x40000000;     // 1 GiB -- different PDPT entry
constexpr uintptr_t high_vaddr = 0x10000000000;  // 1 TiB -- different PML4 entry

inline vm_paddr_t read_cr3() {
    uintptr_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

inline void write_cr3(vm_paddr_t cr3) { asm volatile("mov %0, %%cr3" ::"r"(cr3) : "memory"); }

}  // namespace

KTEST(paging_init_destroy_roundtrip, "mm/paging") {
    vm_aspace space;
    KTEST_REQUIRE_TRUE(space.init());
    KTEST_EXPECT_TRUE(space.is_valid());
    KTEST_EXPECT_TRUE(space.has_root());

    space.destroy();
    KTEST_EXPECT_FALSE(space.is_valid());
    KTEST_EXPECT_FALSE(space.has_root());
}

KTEST(paging_double_init_fails, "mm/paging") {
    vm_aspace space;
    KTEST_REQUIRE_TRUE(space.init());
    KTEST_EXPECT_FALSE(space.init());
}

KTEST(paging_walk_unmapped_returns_nothing, "mm/paging") {
    vm_aspace space;
    KTEST_REQUIRE_TRUE(space.init());
    KTEST_EXPECT_FALSE(space.walk(low_vaddr).has_value());
    KTEST_EXPECT_FALSE(space.walk_ext(low_vaddr).has_value());
}

// Map/walk/unmap round-trip once per protection combination, verifying both the
// physical resolution and that walk_ext recovers the exact prot the mapping was
// installed with. x86_64 has no read-enable bit, so READ is always implied.
KTEST(paging_prot_combinations_roundtrip, "mm/paging") {
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
}

// The no-execute bit must round-trip: a mapping without EXECUTE reports no
// EXECUTE, one with EXECUTE reports it. Proves the old NO_EXECUTE rejection is
// gone and EFER.NXE is honored.
KTEST(paging_nx_is_honored, "mm/paging") {
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

// DEVICE degrades to uncached; walk_ext must report a non-cached attribute.
KTEST(paging_device_cache_mode_reported, "mm/paging") {
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

KTEST(paging_walk_returns_paddr_with_offset, "mm/paging") {
    vm_aspace space;
    KTEST_REQUIRE_TRUE(space.init());
    KTEST_REQUIRE_VALUE(frame, kernel::mm::g_page_frame_allocator.alloc());

    KTEST_REQUIRE_TRUE(space.map_page(low_vaddr, frame, vm_prot::READ | vm_prot::WRITE));
    KTEST_EXPECT_VALUE(space.walk(low_vaddr + 0x123), frame + 0x123);

    KTEST_REQUIRE_TRUE(space.unmap_page(low_vaddr).has_value());
    kernel::mm::g_page_frame_allocator.free(frame);
}

KTEST(paging_double_map_fails, "mm/paging") {
    vm_aspace space;
    KTEST_REQUIRE_TRUE(space.init());
    KTEST_REQUIRE_VALUE(frame, kernel::mm::g_page_frame_allocator.alloc());

    KTEST_REQUIRE_TRUE(space.map_page(low_vaddr, frame, vm_prot::READ | vm_prot::WRITE));
    KTEST_EXPECT_FALSE(space.map_page(low_vaddr, frame, vm_prot::READ | vm_prot::WRITE));

    KTEST_REQUIRE_TRUE(space.unmap_page(low_vaddr).has_value());
    kernel::mm::g_page_frame_allocator.free(frame);
}

KTEST(paging_map_unaligned_fails, "mm/paging") {
    vm_aspace space;
    KTEST_REQUIRE_TRUE(space.init());
    KTEST_REQUIRE_VALUE(frame, kernel::mm::g_page_frame_allocator.alloc());

    KTEST_EXPECT_FALSE(space.map_page(low_vaddr + 1, frame, vm_prot::READ));
    KTEST_EXPECT_FALSE(space.map_page(low_vaddr, frame + 1, vm_prot::READ));

    kernel::mm::g_page_frame_allocator.free(frame);
}

KTEST(paging_independent_mappings_across_levels, "mm/paging") {
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

// A non-canonical address (bit 47 set but sign-extension broken) is rejected by
// every entry point. The bit-47 reasoning stays inside the arch layer.
KTEST(paging_non_canonical_rejected, "mm/paging") {
    vm_aspace space;
    KTEST_REQUIRE_TRUE(space.init());
    KTEST_REQUIRE_VALUE(frame, kernel::mm::g_page_frame_allocator.alloc());

    constexpr uintptr_t non_canonical = 0x0000800000000000ull;
    KTEST_EXPECT_FALSE(space.map_page(non_canonical, frame, vm_prot::READ));
    KTEST_EXPECT_FALSE(space.walk(non_canonical).has_value());
    KTEST_EXPECT_FALSE(space.walk_ext(non_canonical).has_value());
    KTEST_EXPECT_FALSE(space.unmap_page(non_canonical).has_value());

    kernel::mm::g_page_frame_allocator.free(frame);
}

// init() clones the kernel half from the active space, so a fresh space can
// resolve a known kernel-half address (an HHDM offset) without any mapping of
// its own.
KTEST(paging_init_clones_kernel_half, "mm/paging") {
    vm_aspace space;
    KTEST_REQUIRE_TRUE(space.init());

    uintptr_t kaddr = g_hhdm_offset + 0x1000;  // HHDM maps physical 0x1000 here
    KTEST_EXPECT_VALUE(space.walk(kaddr), static_cast<vm_paddr_t>(0x1000));
}

// Mapping over an existing kernel-half mapping fails. On QEMU the HHDM is backed
// by huge pages, so this exercises the huge-intermediate collision path; if the
// bootloader used 4K pages instead it still fails as an already-present leaf.
KTEST(paging_map_over_kernel_mapping_fails, "mm/paging") {
    vm_aspace space;
    KTEST_REQUIRE_TRUE(space.init());
    KTEST_REQUIRE_VALUE(frame, kernel::mm::g_page_frame_allocator.alloc());

    uintptr_t kaddr = (g_hhdm_offset + 0x200000) & ~static_cast<uintptr_t>(0xFFF);
    KTEST_EXPECT_FALSE(space.map_page(kaddr, frame, vm_prot::READ | vm_prot::WRITE));

    kernel::mm::g_page_frame_allocator.free(frame);
}

// A low mapping in one space is invisible to another space: the user half is
// per-space, proving isolation.
KTEST(paging_user_half_is_isolated, "mm/paging") {
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

// Full activation round-trip: create a second space, map into it, load its CR3,
// touch the mapping through its virtual address, then restore the original CR3.
// Requires a clean VM because it switches the live address space.
KTEST_INTEGRATION(paging_activate_and_touch, "mm/paging") {
    vm_aspace space;
    KTEST_REQUIRE_TRUE(space.init());
    KTEST_REQUIRE_VALUE(frame, kernel::mm::g_page_frame_allocator.alloc());

    // Supervisor mapping (no USER bit) so a CPL0 access is never blocked by SMAP.
    KTEST_REQUIRE_TRUE(space.map_page(mid_vaddr, frame, vm_prot::READ | vm_prot::WRITE));

    constexpr uint64_t magic = 0xA5A5C0FFEE00B00Dull;
    vm_paddr_t boot_cr3      = read_cr3();

    // Danger window: no aborting checks between activate() and the CR3 restore.
    space.activate();
    auto* p       = reinterpret_cast<volatile uint64_t*>(mid_vaddr);
    *p            = magic;
    uint64_t seen = *p;
    write_cr3(boot_cr3);

    KTEST_EXPECT_EQUAL(seen, magic);

    KTEST_REQUIRE_TRUE(space.unmap_page(mid_vaddr).has_value());
    kernel::mm::g_page_frame_allocator.free(frame);
}

#endif  // CONFIG_KERNEL_TESTING
