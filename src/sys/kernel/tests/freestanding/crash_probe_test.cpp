#include <kernel/config.h>

#if CONFIG_KERNEL_TESTING

#include <stddef.h>
#include <stdint.h>

#include "kernel/crash.h"
#include "kernel/testing/testing.h"

// HHDM offset published by the Limine boot path (x86_64/main.cpp).
extern uintptr_t g_hhdm_offset;

namespace kernel::crash::arch {
struct fp_walk_result {
    static constexpr size_t max_frames = 32;
    size_t depth                       = 0;
    uintptr_t frames[max_frames]       = {};
};
fp_walk_result walk_frame_pointers(uintptr_t start_rbp);
}  // namespace kernel::crash::arch

namespace { uint64_t g_probe_anchor = 0xa5a5'a5a5'a5a5'a5a5ULL; }  // namespace

KTEST_MODULE("crash_probe");

KTEST_CASE(crash_probe_mapped_global) {
    KTEST_EXPECT_TRUE(kernel::crash::arch::probe_readable(reinterpret_cast<uintptr_t>(&g_probe_anchor)));
}

KTEST_CASE(crash_probe_mapped_stack) {
    volatile uint64_t local = 1;
    KTEST_EXPECT_TRUE(kernel::crash::arch::probe_readable(reinterpret_cast<uintptr_t>(&local)));
}

KTEST_CASE(crash_probe_rejects_non_canonical) {
    KTEST_EXPECT_FALSE(kernel::crash::arch::probe_readable(0xdead'beef'dead'beefULL));
}

KTEST_CASE(crash_probe_rejects_null) { KTEST_EXPECT_FALSE(kernel::crash::arch::probe_readable(0)); }

KTEST_CASE(crash_probe_rejects_far_unmapped) {
    // 64 GiB past the HHDM base: canonical higher-half, but far beyond the test
    // VM's RAM and Limine's 4 GiB direct map, so provably unmapped.
    KTEST_REQUIRE_TRUE(g_hhdm_offset != 0);
    KTEST_EXPECT_FALSE(kernel::crash::arch::probe_readable(g_hhdm_offset + (64ull << 30)));
}

KTEST_CASE(crash_walk_rejects_wrapping_rbp) {
    // Canonical, higher-half, 8-aligned -- but rbp+8 would wrap to 0.
    auto bt = kernel::crash::arch::walk_frame_pointers(0xffff'ffff'ffff'fff8ULL);
    KTEST_EXPECT_EQUAL(bt.depth, static_cast<size_t>(0));
}

KTEST_CASE(crash_walk_rejects_unmapped_rbp) {
    // Plausible-looking rbp pointing into provably unmapped higher-half memory:
    // must terminate the walk instead of dereferencing.
    KTEST_REQUIRE_TRUE(g_hhdm_offset != 0);
    auto bt = kernel::crash::arch::walk_frame_pointers(g_hhdm_offset + (64ull << 30));
    KTEST_EXPECT_EQUAL(bt.depth, static_cast<size_t>(0));
}

KTEST_CASE(crash_walk_fake_chain) {
    // Build a two-frame fp chain on the (mapped, higher-half) stack using the
    // architecture's frame-record layout.
    uintptr_t ret1 = reinterpret_cast<uintptr_t>(&g_probe_anchor);
    uintptr_t ret2 = ret1 + 4;

    uint64_t buf[6];
#ifdef ARCH_X86
    // x86_64: rbp points at the record: [rbp+0] = saved rbp, [rbp+8] = return address.
    buf[0]          = reinterpret_cast<uintptr_t>(&buf[2]);  // frame A: saved rbp -> frame B
    buf[1]          = ret1;
    buf[2]          = reinterpret_cast<uintptr_t>(&buf[4]);  // frame B: saved rbp -> terminator
    buf[3]          = ret2;
    buf[4]          = 0;  // terminator: implausible saved rbp stops the walk
    buf[5]          = 0;
    uintptr_t start = reinterpret_cast<uintptr_t>(&buf[0]);
#elif defined(ARCH_RISCV64)
    // riscv64: fp points one past the record: [fp-8] = return address, [fp-16] = saved fp.
    buf[0]          = reinterpret_cast<uintptr_t>(&buf[4]);  // frame A: saved fp -> frame B
    buf[1]          = ret1;
    buf[2]          = 0;  // frame B: saved fp terminator stops the walk
    buf[3]          = ret2;
    buf[4]          = 0;
    buf[5]          = 0;
    uintptr_t start = reinterpret_cast<uintptr_t>(&buf[2]);
#endif

    auto bt = kernel::crash::arch::walk_frame_pointers(start);
    KTEST_REQUIRE_EQUAL(bt.depth, static_cast<size_t>(2));
    KTEST_EXPECT_EQUAL(bt.frames[0], ret1);
    KTEST_EXPECT_EQUAL(bt.frames[1], ret2);
}

#endif  // CONFIG_KERNEL_TESTING
