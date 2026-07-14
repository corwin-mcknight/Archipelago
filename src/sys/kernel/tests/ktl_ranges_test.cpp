#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <stddef.h>
#include <stdint.h>

#include <ktl/ranges>
#include <ktl/span>
#include <ktl/tuple>
#include <ktl/utility>

using namespace kernel::testing;

// The pair/tuple/span suites below keep their own module names via explicit KTEST.
KTEST_MODULE("ktl/ranges");

// ============================================================
// ktl::pair
// ============================================================

KTEST(ktl_pair_basics, "ktl/pair") {
    ktl::pair<int, int> p(1, 2);
    KTEST_EXPECT_EQUAL(p.first, 1);
    KTEST_EXPECT_EQUAL(p.second, 2);

    // Structured bindings via public members.
    auto [a, b] = p;
    KTEST_EXPECT_EQUAL(a, 1);
    KTEST_EXPECT_EQUAL(b, 2);

    // Reference member aliases external storage (write-through).
    int x = 5;
    ktl::pair<size_t, int&> r((size_t)7, x);
    KTEST_EXPECT_EQUAL(r.first, (size_t)7);
    r.second = 9;
    KTEST_EXPECT_EQUAL(x, 9);

    KTEST_EXPECT_TRUE((ktl::make_pair(1, 2) == ktl::pair<int, int>(1, 2)));
    KTEST_EXPECT_TRUE((ktl::make_pair(1, 2) != ktl::pair<int, int>(1, 3)));
    KTEST_EXPECT_EQUAL(ktl::get<0>(p), 1);
    KTEST_EXPECT_EQUAL(ktl::get<1>(p), 2);
}

// ============================================================
// ktl::tuple
// ============================================================

KTEST(ktl_tuple_basics, "ktl/tuple") {
    ktl::tuple<int, char, int> t(1, 'b', 3);
    KTEST_EXPECT_EQUAL(ktl::get<0>(t), 1);
    KTEST_EXPECT_TRUE(ktl::get<1>(t) == 'b');
    KTEST_EXPECT_EQUAL(ktl::get<2>(t), 3);

    static_assert(ktl::tuple_size<ktl::tuple<int, char, int>>::value == 3);

    // get<Type>
    KTEST_EXPECT_TRUE(ktl::get<char>(t) == 'b');

    // Structured bindings via the tuple protocol.
    auto [a, b, c] = t;
    KTEST_EXPECT_EQUAL(a, 1);
    KTEST_EXPECT_TRUE(b == 'b');
    KTEST_EXPECT_EQUAL(c, 3);

    KTEST_EXPECT_TRUE(ktl::make_tuple(1, 2) == ktl::make_tuple(1, 2));
    KTEST_EXPECT_TRUE(ktl::make_tuple(1, 2) != ktl::make_tuple(1, 9));
}

// ============================================================
// ktl::span
// ============================================================

KTEST(ktl_span_basics, "ktl/span") {
    int arr[5] = {10, 20, 30, 40, 50};

    ktl::span<int> s(arr);  // C-array ctor + class arg
    KTEST_EXPECT_EQUAL(s.size(), (size_t)5);
    KTEST_EXPECT_FALSE(s.empty());
    KTEST_EXPECT_EQUAL(s[0], 10);
    KTEST_EXPECT_EQUAL(s.front(), 10);
    KTEST_EXPECT_EQUAL(s.back(), 50);

    // CTAD from pointer + length.
    auto s2 = ktl::span(arr, 3);
    KTEST_EXPECT_EQUAL(s2.size(), (size_t)3);

    // Range-for + write-through.
    int sum = 0;
    for (int& v : s) { sum += v; }
    KTEST_EXPECT_EQUAL(sum, 150);

    // Slicing clamps.
    KTEST_EXPECT_EQUAL(s.first(2).size(), (size_t)2);
    KTEST_EXPECT_EQUAL(s.first(99).size(), (size_t)5);
    KTEST_EXPECT_EQUAL(s.last(2)[0], 40);
    KTEST_EXPECT_EQUAL(s.subspan(1, 2).size(), (size_t)2);
    KTEST_EXPECT_EQUAL(s.subspan(1, 2)[0], 20);
    KTEST_EXPECT_EQUAL(s.subspan(3, 99).size(), (size_t)2);
    KTEST_EXPECT_EQUAL(s.subspan(99).size(), (size_t)0);
}

// ============================================================
// ktl::ranges
// ============================================================

KTEST_CASE(ktl_views_enumerate) {
    int arr[3]      = {7, 8, 9};
    size_t expect_i = 0;
    int expect_v    = 7;
    for (auto [i, v] : ktl::views::enumerate(ktl::span(arr))) {
        KTEST_EXPECT_EQUAL(i, expect_i);
        KTEST_EXPECT_EQUAL(v, expect_v);
        ++expect_i;
        ++expect_v;
    }
    KTEST_EXPECT_EQUAL(expect_i, (size_t)3);

    // enumerate aliases elements (write-through).
    for (auto [i, v] : ktl::views::enumerate(ktl::span(arr))) { v = (int)i; }
    KTEST_EXPECT_EQUAL(arr[0], 0);
    KTEST_EXPECT_EQUAL(arr[2], 2);
}

KTEST_CASE(ktl_views_filter) {
    int arr[6] = {1, 2, 3, 4, 5, 6};
    int sum    = 0;
    int n      = 0;
    for (int v : ktl::span(arr) | ktl::views::filter([](int x) { return x % 2 == 0; })) {
        sum += v;
        ++n;
    }
    KTEST_EXPECT_EQUAL(n, 3);
    KTEST_EXPECT_EQUAL(sum, 12);

    // No matches -> empty.
    int none = 0;
    for (int v : ktl::span(arr) | ktl::views::filter([](int) { return false; })) { none += v; }
    KTEST_EXPECT_EQUAL(none, 0);
}

KTEST_CASE(ktl_views_transform) {
    int arr[3] = {1, 2, 3};
    int sum    = 0;
    for (int v : ktl::span(arr) | ktl::views::transform([](int x) { return x * 10; })) { sum += v; }
    KTEST_EXPECT_EQUAL(sum, 60);
}

KTEST_CASE(ktl_views_take_drop) {
    int arr[5] = {1, 2, 3, 4, 5};

    int sum    = 0;
    for (int v : ktl::span(arr) | ktl::views::take(2)) { sum += v; }
    KTEST_EXPECT_EQUAL(sum, 3);

    // take beyond size is clamped by the underlying end.
    sum = 0;
    for (int v : ktl::span(arr) | ktl::views::take(99)) { sum += v; }
    KTEST_EXPECT_EQUAL(sum, 15);

    sum = 0;
    for (int v : ktl::span(arr) | ktl::views::drop(3)) { sum += v; }
    KTEST_EXPECT_EQUAL(sum, 9);

    // drop beyond size -> empty.
    sum = 0;
    for (int v : ktl::span(arr) | ktl::views::drop(99)) { sum += v; }
    KTEST_EXPECT_EQUAL(sum, 0);
}

KTEST_CASE(ktl_views_for_each) {
    int arr[6] = {1, 2, 3, 4, 5, 6};

    // Plain range.
    int sum    = 0;
    ktl::for_each(ktl::span(arr), [&](int v) { sum += v; });
    KTEST_EXPECT_EQUAL(sum, 21);

    // filter (internal iteration: same result as the external-iterator form).
    sum = 0;
    ktl::for_each(ktl::span(arr) | ktl::views::filter([](int v) { return v % 2 == 0; }), [&](int v) { sum += v; });
    KTEST_EXPECT_EQUAL(sum, 12);

    // transform.
    sum = 0;
    ktl::for_each(ktl::span(arr) | ktl::views::transform([](int v) { return v * 10; }), [&](int v) { sum += v; });
    KTEST_EXPECT_EQUAL(sum, 210);

    // enumerate | filter -- index preserved through the chain.
    int seen_i = -1, seen_v = -1, count = 0;
    ktl::for_each(
        ktl::span(arr) | ktl::views::enumerate | ktl::views::filter([](const auto& e) { return e.second == 5; }),
        [&](const auto& e) {
            seen_i = (int)e.first;
            seen_v = e.second;
            ++count;
        });
    KTEST_EXPECT_EQUAL(count, 1);
    KTEST_EXPECT_EQUAL(seen_i, 4);
    KTEST_EXPECT_EQUAL(seen_v, 5);

    // drop skips the first N.
    sum = 0;
    ktl::for_each(ktl::span(arr) | ktl::views::drop(4), [&](int v) { sum += v; });
    KTEST_EXPECT_EQUAL(sum, 11);  // 5 + 6

    // take falls back to external iteration but stays correct.
    sum = 0;
    ktl::for_each(ktl::span(arr) | ktl::views::take(2), [&](int v) { sum += v; });
    KTEST_EXPECT_EQUAL(sum, 3);  // 1 + 2

    // Write-through: enumerate yields a reference, for_each can mutate the source.
    ktl::for_each(ktl::span(arr) | ktl::views::enumerate, [](auto&& e) { e.second = (int)e.first; });
    KTEST_EXPECT_EQUAL(arr[0], 0);
    KTEST_EXPECT_EQUAL(arr[5], 5);
}

KTEST_CASE(ktl_views_enumerate_then_filter_keeps_index) {
    // The exact cpu.cpp semantics: enumerate BEFORE filter, so indices stay original.
    int arr[5]       = {10, 20, 30, 40, 50};
    size_t seen_i[2] = {99, 99};
    int seen_v[2]    = {0, 0};
    int k            = 0;
    for (auto [i, v] : ktl::span(arr) | ktl::views::enumerate | ktl::views::filter([](const auto& e) {
                           return e.second >= 40;  // keep 40, 50 -> original indices 3, 4
                       })) {
        seen_i[k] = i;
        seen_v[k] = v;
        ++k;
    }
    KTEST_EXPECT_EQUAL(k, 2);
    KTEST_EXPECT_EQUAL(seen_i[0], (size_t)3);
    KTEST_EXPECT_EQUAL(seen_v[0], 40);
    KTEST_EXPECT_EQUAL(seen_i[1], (size_t)4);
    KTEST_EXPECT_EQUAL(seen_v[1], 50);
}

#endif  // CONFIG_KERNEL_TESTING
