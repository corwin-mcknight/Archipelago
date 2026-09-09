#include <kernel/mm/arch_paging.h>
#include <kernel/mm/paging.h>
#include <kernel/mm/physmap.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vm_aspace.h>
#include <kernel/testing/testing.h>

KTEST_MODULE("mm/paging");

namespace {

using kernel::mm::vm_aspace;
using kernel::mm::vm_cache_mode;
using kernel::mm::vm_paddr_t;
using kernel::mm::vm_prot_t;
namespace vm_prot             = kernel::mm::vm_prot;

constexpr uintptr_t low_vaddr = 0x100000;    // 1 MiB -- user half, empty in a fresh space
constexpr uintptr_t mid_vaddr = 0x40000000;  // 1 GiB -- different second-level entry
// Halfway up the low half: a different top-level entry on both arches (bit 46
// on x86_64, bit 37 on riscv64 Sv39) while staying canonical on each.
uintptr_t high_vaddr() { return vm_aspace::low_limit() / 2; }

uint64_t* root_table(vm_aspace& space) {
    // No aborting checks while the temporary space is active.
    auto* previous = vm_aspace::active();
    space.activate();
    auto root = kernel::mm::arch::current_root();
    previous->activate();
    return reinterpret_cast<uint64_t*>(kernel::mm::direct_map_address(kernel::mm::physical_address(root)));
}

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
// The DEVICE request must also round-trip through the reported cache mode;
// on riscv64 this is a software tag, not a hardware caching guarantee.
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

// Execute permission must round-trip through PTE encoding and decoding when
// replacing a read-write mapping with a read-execute mapping.
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

// Reject duplicate/unaligned maps and non-canonical addresses, while retaining
// access to the final page of the canonical low half.
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

    // This address is non-canonical on both x86_64 and Sv39.
    constexpr uintptr_t non_canonical = 0x0000800000000000ull;
    KTEST_EXPECT_FALSE(space.map_page(non_canonical, frame, vm_prot::READ));
    KTEST_EXPECT_FALSE(space.walk(non_canonical).has_value());
    KTEST_EXPECT_FALSE(space.walk_ext(non_canonical).has_value());
    KTEST_EXPECT_FALSE(space.unmap_page(non_canonical).has_value());

    // The final low-half page remains usable, including supervisor mappings.
    uintptr_t last_low_page = vm_aspace::low_limit() - 0x1000;
    KTEST_REQUIRE_TRUE(space.map_page(last_low_page, frame, vm_prot::READ));
    KTEST_EXPECT_VALUE(space.walk(last_low_page), frame);
    KTEST_EXPECT_VALUE(space.unmap_page(last_low_page), frame);
    KTEST_EXPECT_FALSE(space.map_page(vm_aspace::low_limit(), frame, vm_prot::READ));
    KTEST_EXPECT_FALSE(space.unmap_page(vm_aspace::low_limit()).has_value());

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
    KTEST_REQUIRE_TRUE(space.map_page(high_vaddr(), p3, vm_prot::READ | vm_prot::WRITE));

    KTEST_EXPECT_VALUE(space.walk(low_vaddr), p1);
    KTEST_EXPECT_VALUE(space.walk(mid_vaddr), p2);
    KTEST_EXPECT_VALUE(space.walk(high_vaddr()), p3);

    kernel::mm::g_page_frame_allocator.free(p1);
    kernel::mm::g_page_frame_allocator.free(p2);
    kernel::mm::g_page_frame_allocator.free(p3);
}

// init() clones the kernel half from the active space, so a fresh space can
// resolve a known kernel-half address (an HHDM offset) without any mapping of
// its own -- and mapping over that existing kernel-half mapping fails.
KTEST_CASE(paging_kernel_half_cloned) {
    vm_aspace space;
    KTEST_REQUIRE_TRUE(space.init());

    uintptr_t kaddr = kernel::mm::direct_map_address(kernel::mm::physical_address(0x1000));
    KTEST_EXPECT_VALUE(space.walk(kaddr), static_cast<vm_paddr_t>(0x1000));

    KTEST_REQUIRE_VALUE(frame, kernel::mm::g_page_frame_allocator.alloc());
    uintptr_t hhdm_kaddr =
        kernel::mm::direct_map_address(kernel::mm::physical_address(0x200000)) & ~static_cast<uintptr_t>(0xFFF);
    KTEST_EXPECT_FALSE(space.map_page(hhdm_kaddr, frame, vm_prot::READ | vm_prot::WRITE));

    kernel::mm::g_page_frame_allocator.free(frame);
}

// A synthetic kernel subtree shares source's low-half tables, so attempted
// mutations exercise both existing 4K leaves and holes without depending on
// the bootloader's page sizes or risking a live kernel mapping.
KTEST_CASE(paging_kernel_half_mutations_rejected) {
    namespace arch = kernel::mm::arch;
    vm_aspace source, space;
    KTEST_REQUIRE_TRUE(source.init());
    KTEST_REQUIRE_TRUE(space.init());
    KTEST_REQUIRE_VALUE(frame, kernel::mm::g_page_frame_allocator.alloc());
    KTEST_REQUIRE_TRUE(source.map_page(low_vaddr, frame, vm_prot::READ));

    auto* source_root = root_table(source);
    auto* space_root  = root_table(space);

    // Snapshot the supervisor path: on x86_64 a duplicate USER map could widen
    // shared intermediates before discovering that the leaf is occupied.
    uint64_t* slots[arch::PT_LEVELS - 1];
    uint64_t entries[arch::PT_LEVELS - 1];
    auto* table = source_root;
    for (int level = 0; level < arch::PT_LEVELS - 1; ++level) {
        size_t index   = (low_vaddr >> (arch::VA_BITS - 9 - 9 * level)) & 0x1FF;
        slots[level]   = &table[index];
        entries[level] = *slots[level];
        table          = reinterpret_cast<uint64_t*>(
            kernel::mm::direct_map_address(kernel::mm::physical_address(arch::pte_addr(entries[level]))));
    }

    uintptr_t kernel_base = ~(vm_aspace::low_limit() - 1);
    uintptr_t mapped      = kernel_base + low_vaddr;
    auto saved            = space_root[256];
    space_root[256]       = source_root[0];
    // Keep space inactive until its root is restored. Avoid aborting checks
    // here, including KTEST_EXPECT_VALUE, so restoration always runs.
    auto mapped_before    = space.walk(mapped);
    KTEST_EXPECT_TRUE(mapped_before.has_value());
    if (mapped_before.has_value()) { KTEST_EXPECT_EQUAL(mapped_before.value(), frame); }
    auto before = space.walk_ext(mapped);
    KTEST_EXPECT_TRUE(before.has_value());
    size_t free_before = kernel::mm::g_page_frame_allocator.free_pages();
    KTEST_EXPECT_FALSE(space.map_page(mapped, frame, vm_prot::READ | vm_prot::USER));
    KTEST_EXPECT_FALSE(space.map_page(mapped + 0x1000, frame, vm_prot::READ));
    KTEST_EXPECT_FALSE(space.map_page(mapped + 0x1000, frame, vm_prot::READ | vm_prot::USER));
    // This hole requires a new intermediate table if the guard is missing.
    KTEST_EXPECT_FALSE(space.map_page(kernel_base + 0x200000, frame, vm_prot::READ));
    KTEST_EXPECT_FALSE(space.map_page(kernel_base, frame, vm_prot::READ));
    KTEST_EXPECT_FALSE(space.unmap_page(mapped).has_value());
    KTEST_EXPECT_FALSE(space.unmap_page(mapped + 0x1000).has_value());
    KTEST_EXPECT_EQUAL(kernel::mm::g_page_frame_allocator.free_pages(), free_before);
    auto mapped_after = space.walk(mapped);
    KTEST_EXPECT_TRUE(mapped_after.has_value());
    if (mapped_after.has_value()) { KTEST_EXPECT_EQUAL(mapped_after.value(), frame); }
    KTEST_EXPECT_FALSE(space.walk(mapped + 0x1000).has_value());
    KTEST_EXPECT_FALSE(space.walk(kernel_base + 0x200000).has_value());
    KTEST_EXPECT_FALSE(space.walk(kernel_base).has_value());
    auto after = space.walk_ext(mapped);
    KTEST_EXPECT_TRUE(after.has_value());
    if (before.has_value() && after.has_value()) {
        KTEST_EXPECT_EQUAL(after.value().paddr, before.value().paddr);
        KTEST_EXPECT_EQUAL(after.value().prot, before.value().prot);
        KTEST_EXPECT_TRUE(after.value().cache == before.value().cache);
    }
    KTEST_EXPECT_EQUAL(space_root[256], entries[0]);
    for (int level = 0; level < arch::PT_LEVELS - 1; ++level) { KTEST_EXPECT_EQUAL(*slots[level], entries[level]); }
    space_root[256] = saved;

    KTEST_EXPECT_VALUE(source.walk(low_vaddr), frame);
    // Source owns the shared subtree, including any tables a regression added.
    source.destroy();
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
KTEST_CASE(paging_activate_and_touch) {
    vm_aspace space;
    KTEST_REQUIRE_TRUE(space.init());
    KTEST_REQUIRE_VALUE(frame, kernel::mm::g_page_frame_allocator.alloc());

    // Use supervisor permissions so access does not require user-memory access
    // overrides (x86 SMAP or riscv64 SUM).
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
