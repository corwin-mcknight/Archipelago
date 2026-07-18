#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/testing/tracking_value.h>

#include <ktl/maybe>
#include <ktl/vector>

using namespace kernel::testing;

KTEST_MODULE("ktl/vector");

KTEST_CASE(ktl_vector_push_and_index) {
    ktl::vector<int> vec;
    KTEST_REQUIRE_TRUE(vec.empty());

    KTEST_REQUIRE_TRUE(vec.push_back(1));
    KTEST_REQUIRE_TRUE(vec.push_back(2));
    KTEST_REQUIRE_TRUE(vec.push_back(3));

    KTEST_EXPECT_ALL(vec.size() == 3, vec.capacity() >= vec.size());
    KTEST_EXPECT_EQUAL(vec[0], 1);
    KTEST_EXPECT_EQUAL(vec[1], 2);
    KTEST_EXPECT_EQUAL(vec[2], 3);

    KTEST_EXPECT_VALUE(vec.front(), 1);
    KTEST_EXPECT_VALUE(vec.back(), 3);
    KTEST_EXPECT_FALSE(vec.at(10).has_value());
}

KTEST_CASE(ktl_vector_pop_and_clear) {
    ktl::vector<int> vec;
    for (int i = 0; i < 5; ++i) { KTEST_REQUIRE_TRUE(vec.push_back(i)); }

    KTEST_EXPECT_VALUE(vec.pop_back(), 4);
    KTEST_EXPECT_TRUE(vec.size() == 4);

    vec.clear();
    KTEST_EXPECT_ALL(vec.size() == 0, vec.empty());
    KTEST_EXPECT_FALSE(vec.pop_back().has_value());
}

KTEST_CASE(ktl_vector_swap_remove) {
    ktl::vector<int> vec;
    for (int i = 0; i < 4; ++i) { KTEST_REQUIRE_TRUE(vec.push_back(i)); }

    vec.swap_remove(1);  // middle: last element moves in
    KTEST_EXPECT_ALL(vec.size() == 3, vec[0] == 0, vec[1] == 3, vec[2] == 2);

    vec.swap_remove(10);  // out of range: ignored
    KTEST_EXPECT_TRUE(vec.size() == 3);

    // tracking_value's move-assignment corrupts on self-move, so this fails if
    // removing the last element routes through a self-move.
    ktl::vector<tracking_value> tracked;
    KTEST_REQUIRE_TRUE(tracked.push_back(tracking_value{1}));
    KTEST_REQUIRE_TRUE(tracked.push_back(tracking_value{2}));
    tracked.swap_remove(1);
    KTEST_EXPECT_ALL(tracked.size() == 1, tracked[0].value == 1);
    tracked.swap_remove(0);
    KTEST_EXPECT_TRUE(tracked.empty());
}

KTEST_CASE(ktl_vector_reserve_grows) {
    ktl::vector<int> vec;
    KTEST_REQUIRE_TRUE(vec.reserve(16));
    KTEST_EXPECT_TRUE(vec.capacity() >= 16);

    for (int i = 0; i < 10; ++i) { KTEST_REQUIRE_TRUE(vec.push_back(i)); }
    KTEST_EXPECT_ALL(vec.size() == 10, vec.capacity() >= 16);
}

KTEST_CASE(ktl_vector_move_semantics) {
    ktl::vector<tracking_value> vec;
    tracking_value original{42};

    KTEST_REQUIRE_TRUE(vec.emplace_back(ktl::move(original)));
    KTEST_EXPECT_ALL(original.value == -1, original.move_observed);

    ktl::vector<tracking_value> moved = ktl::move(vec);
    KTEST_EXPECT_ALL(vec.size() == 0, moved.size() == 1);
    KTEST_EXPECT_EQUAL(moved[0].value, 42);
    KTEST_EXPECT_TRUE(moved[0].move_observed);
}

KTEST_CASE(ktl_vector_iterators) {
    ktl::vector<int> vec;
    for (int i = 0; i < 8; ++i) { KTEST_REQUIRE_TRUE(vec.push_back(i)); }

    int expected = 0;
    for (auto it = vec.begin(); it != vec.end(); ++it) {
        KTEST_EXPECT_EQUAL(*it, expected);
        ++expected;
    }
    KTEST_EXPECT_EQUAL(expected, 8);

    ktl::vector<int>::const_iterator converted = vec.begin();
    KTEST_EXPECT_TRUE(converted == vec.begin());

    const auto& const_ref = vec;
    KTEST_EXPECT_EQUAL(static_cast<size_t>(const_ref.end() - const_ref.begin()), const_ref.size());
    KTEST_EXPECT_EQUAL(*(const_ref.begin() + 3), 3);
    KTEST_EXPECT_ALL(const_ref.cbegin() == const_ref.begin(), const_ref.cend() == const_ref.end());
}

KTEST_CASE(ktl_vector_range_for) {
    ktl::vector<int> vec;
    for (int i = 0; i < 6; ++i) { KTEST_REQUIRE_TRUE(vec.push_back(i)); }

    int doubled_sum = 0;
    for (auto& value : vec) {
        value *= 2;
        doubled_sum += value;
    }
    KTEST_EXPECT_EQUAL(doubled_sum, 30);

    const auto& const_ref = vec;
    int expected          = 0;
    for (const auto& value : const_ref) {
        KTEST_EXPECT_EQUAL(value, expected * 2);
        ++expected;
    }
    KTEST_EXPECT_EQUAL(static_cast<size_t>(expected), const_ref.size());
}

KTEST_CASE(ktl_vector_growth_preserves_elements) {
    // Push far past the initial capacity so several reallocations happen mid-fill.
    ktl::vector<int> vec;
    for (int i = 0; i < 100; ++i) { KTEST_REQUIRE_TRUE(vec.push_back(i)); }

    KTEST_REQUIRE_TRUE(vec.size() == 100);
    for (int i = 0; i < 100; ++i) { KTEST_EXPECT_EQUAL(vec[static_cast<size_t>(i)], i); }
}

KTEST_CASE(ktl_vector_non_default_constructible) {
    struct wrapped {
        explicit wrapped(int v) : value(v) {}
        int value;
    };

    ktl::vector<wrapped> vec;
    for (int i = 0; i < 10; ++i) { KTEST_REQUIRE_TRUE(vec.push_back(wrapped(i))); }

    KTEST_REQUIRE_TRUE(vec.size() == 10);
    KTEST_EXPECT_EQUAL(vec[9].value, 9);
}

KTEST_CASE(ktl_vector_const_index) {
    ktl::vector<int> vec;
    KTEST_REQUIRE_TRUE(vec.push_back(7));
    const ktl::vector<int>& cref = vec;
    KTEST_EXPECT_EQUAL(cref[0], 7);
}

#endif  // CONFIG_KERNEL_TESTING
