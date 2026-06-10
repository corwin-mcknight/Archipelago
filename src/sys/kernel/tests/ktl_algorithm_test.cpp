#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <stddef.h>
#include <stdint.h>

#include <ktl/algorithm>
#include <ktl/maybe>

using namespace kernel::testing;

KTEST(ktl_algorithm_find_if_returns_reference, "ktl/algorithm") {
    int values[4] = {2, 4, 7, 8};

    auto found    = ktl::find_if(values, values + 4, [](int v) { return v > 5; });
    KTEST_REQUIRE_TRUE(found.has_value());
    KTEST_EXPECT_EQUAL(found.value(), 7);

    // The result aliases the element: writes land in the range.
    found.value() = 9;
    KTEST_EXPECT_EQUAL(values[2], 9);

    auto missing = ktl::find_if(values, values + 4, [](int v) { return v > 100; });
    KTEST_EXPECT_FALSE(missing.has_value());
}

KTEST(ktl_algorithm_find_by_value, "ktl/algorithm") {
    const int values[3] = {1, 2, 3};

    auto found          = ktl::find(values, values + 3, 2);
    KTEST_REQUIRE_TRUE(found.has_value());
    KTEST_EXPECT_TRUE(&found.value() == &values[1]);

    KTEST_EXPECT_FALSE(ktl::find(values, values + 3, 99).has_value());
}

KTEST(ktl_algorithm_find_index_if_chains, "ktl/algorithm") {
    const uint32_t ids[4] = {10, 20, 30, 40};

    auto idx              = ktl::find_index_if(ids, ids + 4, [](uint32_t id) { return id == 30; });
    KTEST_REQUIRE_TRUE(idx.has_value());
    KTEST_EXPECT_EQUAL(idx.value(), (size_t)2);

    // The find-position-then-transform shape stays one expression.
    auto doubled =
        ktl::find_index_if(ids, ids + 4, [](uint32_t id) { return id == 40; }).map([](size_t i) { return i * 2; });
    KTEST_EXPECT_VALUE(doubled, (size_t)6);

    KTEST_EXPECT_FALSE(ktl::find_index_if(ids, ids + 4, [](uint32_t id) { return id == 99; }).has_value());
}

KTEST(ktl_algorithm_find_if_empty_range, "ktl/algorithm") {
    int* none = nullptr;
    KTEST_EXPECT_FALSE(ktl::find_if(none, none, [](int) { return true; }).has_value());
    KTEST_EXPECT_FALSE(ktl::find_index_if(none, none, [](int) { return true; }).has_value());
}

#endif  // CONFIG_KERNEL_TESTING
