#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <ktl/atomic>

using namespace kernel::testing;

KTEST_MODULE("ktl/atomic");

KTEST_CASE(ktl_atomic_construct_store_load) {
    ktl::atomic<uint32_t> zeroed;
    KTEST_EXPECT_TRUE(zeroed.load() == 0);

    ktl::atomic<uint32_t> initialized{42};
    KTEST_EXPECT_TRUE(initialized.load() == 42);

    initialized.store(99);
    KTEST_EXPECT_TRUE(initialized.load() == 99);
}

// Every read-modify-write returns the previous value and applies the new one.
KTEST_CASE(ktl_atomic_rmw_returns_previous) {
    ktl::atomic<uint32_t> add{10};
    KTEST_EXPECT_ALL(add.fetch_add(5) == 10, add.load() == 15);

    ktl::atomic<uint32_t> sub{10};
    KTEST_EXPECT_ALL(sub.fetch_sub(3) == 10, sub.load() == 7);

    ktl::atomic<uint32_t> ored{0x0F};
    KTEST_EXPECT_ALL(ored.fetch_or(0xF0) == 0x0F, ored.load() == 0xFF);

    ktl::atomic<uint32_t> anded{0xFF};
    KTEST_EXPECT_ALL(anded.fetch_and(0x0F) == 0xFF, anded.load() == 0x0F);

    ktl::atomic<uint32_t> exchanged{42};
    KTEST_EXPECT_ALL(exchanged.exchange(99) == 42, exchanged.load() == 99);
}

// 64-bit values must round-trip whole, not truncate to 32 bits.
KTEST_CASE(ktl_atomic_uint64) {
    ktl::atomic<uint64_t> a{0};
    a.store(0xDEADBEEFCAFE);
    KTEST_EXPECT_TRUE(a.load() == 0xDEADBEEFCAFE);
    uint64_t prev = a.fetch_add(1);
    KTEST_EXPECT_TRUE(prev == 0xDEADBEEFCAFE);
    KTEST_EXPECT_TRUE(a.load() == 0xDEADBEEFCAFF);
}

#endif
