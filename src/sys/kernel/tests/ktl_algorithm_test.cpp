#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <stddef.h>
#include <stdint.h>

#include <ktl/algorithm>
#include <ktl/maybe>
#include <ktl/vector>

using namespace kernel::testing;

KTEST_MODULE("ktl/algorithm");

// The find family: find_if / find / find_index_if all return an empty maybe on a
// miss or an empty range, and a hit that refers back into the searched range.
KTEST_CASE(ktl_algorithm_find_family) {
    int values[4] = {2, 4, 7, 8};

    auto found    = ktl::find_if(values, values + 4, [](int v) { return v > 5; });
    KTEST_REQUIRE_TRUE(found.has_value());
    KTEST_EXPECT_EQUAL(found.value(), 7);

    // The result aliases the element: writes land in the range.
    found.value() = 9;
    KTEST_EXPECT_EQUAL(values[2], 9);

    auto missing = ktl::find_if(values, values + 4, [](int v) { return v > 100; });
    KTEST_EXPECT_FALSE(missing.has_value());

    // find by value returns a reference to the matching element.
    const int by_value[3] = {1, 2, 3};

    auto hit              = ktl::find(by_value, by_value + 3, 2);
    KTEST_REQUIRE_TRUE(hit.has_value());
    KTEST_EXPECT_TRUE(&hit.value() == &by_value[1]);

    KTEST_EXPECT_FALSE(ktl::find(by_value, by_value + 3, 99).has_value());

    // find_index_if yields the position, and chains through maybe.
    const uint32_t ids[4] = {10, 20, 30, 40};

    auto idx              = ktl::find_index_if(ids, ids + 4, [](uint32_t id) { return id == 30; });
    KTEST_REQUIRE_TRUE(idx.has_value());
    KTEST_EXPECT_EQUAL(idx.value(), (size_t)2);

    // The find-position-then-transform shape stays one expression.
    auto doubled =
        ktl::find_index_if(ids, ids + 4, [](uint32_t id) { return id == 40; }).map([](size_t i) { return i * 2; });
    KTEST_EXPECT_VALUE(doubled, (size_t)6);

    KTEST_EXPECT_FALSE(ktl::find_index_if(ids, ids + 4, [](uint32_t id) { return id == 99; }).has_value());

    // Empty ranges find nothing.
    int* none = nullptr;
    KTEST_EXPECT_FALSE(ktl::find_if(none, none, [](int) { return true; }).has_value());
    KTEST_EXPECT_FALSE(ktl::find_index_if(none, none, [](int) { return true; }).has_value());
}

KTEST_CASE(ktl_algorithm_size) {
    // Built-in arrays report their extent, usable at compile time.
    static constexpr int values[5] = {1, 2, 3, 4, 5};
    static_assert(ktl::size(values) == 5);
    KTEST_EXPECT_EQUAL(ktl::size(values), (size_t)5);

    // Containers defer to their size() member.
    ktl::vector<uint32_t> vec;
    vec.push_back(10);
    vec.push_back(20);
    KTEST_EXPECT_EQUAL(ktl::size(vec), (size_t)2);
}

#endif  // CONFIG_KERNEL_TESTING
