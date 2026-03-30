#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/testing/tracking_value.h>

#include <ktl/maybe>
#include <ktl/vector>

using namespace kernel::testing;

KTEST(ktl_vector_push_and_index, "ktl/vector") {
    ktl::vector<int> vec;

    KTEST_REQUIRE_TRUE(vec.empty());

    KTEST_REQUIRE_TRUE(vec.push_back(1));
    KTEST_REQUIRE_TRUE(vec.push_back(2));
    KTEST_REQUIRE_TRUE(vec.push_back(3));

    KTEST_EXPECT_TRUE(vec.size() == 3);
    KTEST_EXPECT_TRUE(vec.capacity() >= vec.size());

    KTEST_EXPECT_EQUAL(vec[0], 1);
    KTEST_EXPECT_EQUAL(vec[1], 2);
    KTEST_EXPECT_EQUAL(vec[2], 3);

    auto first = vec.front();
    auto last  = vec.back();
    KTEST_REQUIRE_TRUE(first.has_value());
    KTEST_REQUIRE_TRUE(last.has_value());
    KTEST_EXPECT_EQUAL(first.value(), 1);
    KTEST_EXPECT_EQUAL(last.value(), 3);

    auto out_of_range = vec.at(10);
    KTEST_EXPECT_FALSE(out_of_range.has_value());
}

KTEST(ktl_vector_pop_and_clear, "ktl/vector") {
    ktl::vector<int> vec;
    for (int i = 0; i < 5; ++i) { KTEST_REQUIRE_TRUE(vec.push_back(i)); }

    auto popped = vec.pop_back();
    KTEST_REQUIRE_TRUE(popped.has_value());
    KTEST_EXPECT_EQUAL(popped.value(), 4);
    KTEST_EXPECT_TRUE(vec.size() == 4);

    vec.clear();
    KTEST_EXPECT_TRUE(vec.size() == 0);
    KTEST_EXPECT_TRUE(vec.empty());

    auto empty_pop = vec.pop_back();
    KTEST_EXPECT_FALSE(empty_pop.has_value());
}

KTEST(ktl_vector_reserve_grows, "ktl/vector") {
    ktl::vector<int> vec;

    KTEST_REQUIRE_TRUE(vec.reserve(16));
    KTEST_EXPECT_TRUE(vec.capacity() >= 16);

    for (int i = 0; i < 10; ++i) { KTEST_REQUIRE_TRUE(vec.push_back(i)); }
    KTEST_EXPECT_TRUE(vec.size() == 10);
    KTEST_EXPECT_TRUE(vec.capacity() >= 16);
}

KTEST(ktl_vector_move_semantics, "ktl/vector") {
    ktl::vector<tracking_value> vec;
    tracking_value original{42};

    KTEST_REQUIRE_TRUE(vec.emplace_back(ktl::move(original)));
    KTEST_EXPECT_TRUE(original.value == -1);
    KTEST_EXPECT_TRUE(original.move_observed);

    ktl::vector<tracking_value> moved = ktl::move(vec);
    KTEST_EXPECT_TRUE(vec.size() == 0);
    KTEST_EXPECT_TRUE(moved.size() == 1);
    KTEST_EXPECT_EQUAL(moved[0].value, 42);
    KTEST_EXPECT_TRUE(moved[0].move_observed);
}

KTEST(ktl_vector_iterators, "ktl/vector") {
    ktl::vector<int> vec;

    for (int i = 0; i < 8; ++i) { KTEST_REQUIRE_TRUE(vec.push_back(i)); }

    int expected = 0;
    for (auto it = vec.begin(); it != vec.end(); ++it) {
        KTEST_EXPECT_EQUAL(*it, expected);
        ++expected;
    }
    KTEST_EXPECT_EQUAL(expected, 8);

    auto mutable_begin                         = vec.begin();
    ktl::vector<int>::const_iterator converted = mutable_begin;
    KTEST_EXPECT_TRUE(converted == vec.begin());

    const auto& const_ref = vec;
    KTEST_EXPECT_EQUAL(static_cast<size_t>(const_ref.end() - const_ref.begin()), const_ref.size());
    KTEST_EXPECT_EQUAL(*(const_ref.begin() + 3), 3);

    auto cbegin = const_ref.cbegin();
    auto cend   = const_ref.cend();
    KTEST_EXPECT_TRUE(cbegin == const_ref.begin());
    KTEST_EXPECT_TRUE(cend == const_ref.end());
}

KTEST(ktl_vector_range_for, "ktl/vector") {
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

#endif  // CONFIG_KERNEL_TESTING
