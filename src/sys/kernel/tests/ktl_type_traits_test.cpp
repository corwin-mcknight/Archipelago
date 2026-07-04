#include <kernel/testing/testing.h>

#include <ktl/type_traits>

#if CONFIG_KERNEL_TESTING

using namespace kernel::testing;

// decay_t follows std::decay: cv/ref stripping, array-to-pointer, function-to-pointer.
static_assert(ktl::is_same_v<ktl::decay_t<int>, int>);
static_assert(ktl::is_same_v<ktl::decay_t<const int&>, int>);
static_assert(ktl::is_same_v<ktl::decay_t<volatile int&&>, int>);
static_assert(ktl::is_same_v<ktl::decay_t<int[4]>, int*>);
static_assert(ktl::is_same_v<ktl::decay_t<int[]>, int*>);
static_assert(ktl::is_same_v<ktl::decay_t<const char[6]>, const char*>);
static_assert(ktl::is_same_v<ktl::decay_t<int (&)[4]>, int*>);
static_assert(ktl::is_same_v<ktl::decay_t<int(char)>, int (*)(char)>);
static_assert(ktl::is_same_v<ktl::decay_t<int (&)(char)>, int (*)(char)>);

KTEST(ktl_type_traits_decay, "ktl/type_traits") {
    // All checks are the compile-time static_asserts above; reaching here means they held.
    KTEST_EXPECT_TRUE(true);
}

#endif
