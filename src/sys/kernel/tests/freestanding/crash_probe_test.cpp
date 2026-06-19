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

KTEST(crash_probe_mapped_global, "crash_probe") {
    KTEST_EXPECT_TRUE(kernel::crash::arch::probe_readable(reinterpret_cast<uintptr_t>(&g_probe_anchor)));
}

KTEST(crash_probe_mapped_stack, "crash_probe") {
    volatile uint64_t local = 1;
    KTEST_EXPECT_TRUE(kernel::crash::arch::probe_readable(reinterpret_cast<uintptr_t>(&local)));
}

KTEST(crash_probe_rejects_non_canonical, "crash_probe") {
    KTEST_EXPECT_FALSE(kernel::crash::arch::probe_readable(0xdead'beef'dead'beefULL));
}

KTEST(crash_probe_rejects_null, "crash_probe") { KTEST_EXPECT_FALSE(kernel::crash::arch::probe_readable(0)); }

KTEST(crash_probe_rejects_far_unmapped, "crash_probe") {
    // 64 GiB past the HHDM base: canonical higher-half, but far beyond the test
    // VM's RAM and Limine's 4 GiB direct map, so provably unmapped.
    KTEST_REQUIRE_TRUE(g_hhdm_offset != 0);
    KTEST_EXPECT_FALSE(kernel::crash::arch::probe_readable(g_hhdm_offset + (64ull << 30)));
}

KTEST(crash_walk_rejects_wrapping_rbp, "crash_probe") {
    // Canonical, higher-half, 8-aligned -- but rbp+8 would wrap to 0 (F022).
    auto bt = kernel::crash::arch::walk_frame_pointers(0xffff'ffff'ffff'fff8ULL);
    KTEST_EXPECT_EQUAL(bt.depth, static_cast<size_t>(0));
}

KTEST(crash_walk_rejects_unmapped_rbp, "crash_probe") {
    // Plausible-looking rbp pointing into provably unmapped higher-half memory:
    // must terminate the walk instead of dereferencing (F022).
    KTEST_REQUIRE_TRUE(g_hhdm_offset != 0);
    auto bt = kernel::crash::arch::walk_frame_pointers(g_hhdm_offset + (64ull << 30));
    KTEST_EXPECT_EQUAL(bt.depth, static_cast<size_t>(0));
}

KTEST(crash_walk_fake_chain, "crash_probe") {
    // Build a two-frame fp chain on the (mapped, higher-half) stack:
    // frame layout is [rbp+0] = saved rbp, [rbp+8] = return address.
    uintptr_t ret1 = reinterpret_cast<uintptr_t>(&g_probe_anchor);
    uintptr_t ret2 = ret1 + 4;

    uint64_t buf[6];
    buf[0]  = reinterpret_cast<uintptr_t>(&buf[2]);  // frame A: saved rbp -> frame B
    buf[1]  = ret1;
    buf[2]  = reinterpret_cast<uintptr_t>(&buf[4]);  // frame B: saved rbp -> terminator
    buf[3]  = ret2;
    buf[4]  = 0;  // terminator: implausible saved rbp stops the walk
    buf[5]  = 0;

    auto bt = kernel::crash::arch::walk_frame_pointers(reinterpret_cast<uintptr_t>(&buf[0]));
    KTEST_REQUIRE_EQUAL(bt.depth, static_cast<size_t>(2));
    KTEST_EXPECT_EQUAL(bt.frames[0], ret1);
    KTEST_EXPECT_EQUAL(bt.frames[1], ret2);
}

#endif  // CONFIG_KERNEL_TESTING
