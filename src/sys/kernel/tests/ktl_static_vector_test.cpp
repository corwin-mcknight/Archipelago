#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <ktl/static_vector>

using namespace kernel::testing;

KTEST(ktl_static_vector_at_bounds_on_size_not_capacity, "ktl/static_vector") {
    // Capacity 8, only two elements pushed: indices >= size must be nothing,
    // even though they are < capacity.
    ktl::static_vector<int, 8> v;
    v.push_back(10);
    v.push_back(20);

    KTEST_EXPECT_TRUE(v.at(1).value_or(-1) == 20);
    KTEST_EXPECT_TRUE(!v.at(2).has_value());
    KTEST_EXPECT_TRUE(!v.at(5).has_value());
}

#endif  // CONFIG_KERNEL_TESTING
