#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <ktl/atomic>

using namespace kernel::testing;

KTEST(ktl_atomic_default_zero, "ktl/atomic") {
    ktl::atomic<uint32_t> a;
    KTEST_EXPECT_TRUE(a.load() == 0);
}

KTEST(ktl_atomic_initial_value, "ktl/atomic") {
    ktl::atomic<uint32_t> a{42};
    KTEST_EXPECT_TRUE(a.load() == 42);
}

KTEST(ktl_atomic_store_load, "ktl/atomic") {
    ktl::atomic<uint32_t> a{0};
    a.store(99);
    KTEST_EXPECT_TRUE(a.load() == 99);
}

KTEST(ktl_atomic_fetch_add, "ktl/atomic") {
    ktl::atomic<uint32_t> a{10};
    uint32_t prev = a.fetch_add(5);
    KTEST_EXPECT_TRUE(prev == 10);
    KTEST_EXPECT_TRUE(a.load() == 15);
}

KTEST(ktl_atomic_fetch_sub, "ktl/atomic") {
    ktl::atomic<uint32_t> a{10};
    uint32_t prev = a.fetch_sub(3);
    KTEST_EXPECT_TRUE(prev == 10);
    KTEST_EXPECT_TRUE(a.load() == 7);
}

KTEST(ktl_atomic_fetch_or, "ktl/atomic") {
    ktl::atomic<uint32_t> a{0x0F};
    uint32_t prev = a.fetch_or(0xF0);
    KTEST_EXPECT_TRUE(prev == 0x0F);
    KTEST_EXPECT_TRUE(a.load() == 0xFF);
}

KTEST(ktl_atomic_fetch_and, "ktl/atomic") {
    ktl::atomic<uint32_t> a{0xFF};
    uint32_t prev = a.fetch_and(0x0F);
    KTEST_EXPECT_TRUE(prev == 0xFF);
    KTEST_EXPECT_TRUE(a.load() == 0x0F);
}

KTEST(ktl_atomic_exchange, "ktl/atomic") {
    ktl::atomic<uint32_t> a{42};
    uint32_t old = a.exchange(99);
    KTEST_EXPECT_TRUE(old == 42);
    KTEST_EXPECT_TRUE(a.load() == 99);
}

KTEST(ktl_atomic_uint64, "ktl/atomic") {
    ktl::atomic<uint64_t> a{0};
    a.store(0xDEADBEEFCAFE);
    KTEST_EXPECT_TRUE(a.load() == 0xDEADBEEFCAFE);
    uint64_t prev = a.fetch_add(1);
    KTEST_EXPECT_TRUE(prev == 0xDEADBEEFCAFE);
    KTEST_EXPECT_TRUE(a.load() == 0xDEADBEEFCAFF);
}

#endif
