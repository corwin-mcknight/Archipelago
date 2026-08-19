#include <kernel/testing/testing.h>
#include <stddef.h>
#include <stdint.h>

#include <ktl/ranges>
#include <ktl/span>
#include <ktl/utility>

using namespace kernel::testing;

// The span suite below keeps its own module name via explicit KTEST.
KTEST_MODULE("ktl/ranges");

// ============================================================
// ktl::span
// ============================================================

KTEST(ktl_span_basics, "ktl/span") {
    int arr[5] = {10, 20, 30, 40, 50};

    ktl::span<int> s(arr);  // C-array ctor + class arg
    KTEST_EXPECT_ALL(s.size() == (size_t)5, !s.empty(), s[0] == 10, s[4] == 50);

    // CTAD from pointer + length.
    auto s2 = ktl::span(arr, 3);
    KTEST_EXPECT_EQUAL(s2.size(), (size_t)3);

    // Range-for + write-through.
    int sum = 0;
    for (int& v : s) { sum += v; }
    KTEST_EXPECT_EQUAL(sum, 150);

    // Slicing clamps.
    KTEST_EXPECT_ALL(s.first(2).size() == (size_t)2, s.first(2)[1] == 20, s.first(99).size() == (size_t)5);
}

// ============================================================
// ktl::ranges
// ============================================================

KTEST_CASE(ktl_views_enumerate) {
    int arr[3]      = {7, 8, 9};
    size_t expect_i = 0;
    int expect_v    = 7;
    for (auto [i, v] : ktl::views::enumerate(ktl::span(arr))) {
        KTEST_EXPECT_ALL(i == expect_i, v == expect_v);
        ++expect_i;
        ++expect_v;
    }
    KTEST_EXPECT_EQUAL(expect_i, (size_t)3);

    // enumerate aliases elements (write-through).
    for (auto [i, v] : ktl::views::enumerate(ktl::span(arr))) { v = (int)i; }
    KTEST_EXPECT_ALL(arr[0] == 0, arr[2] == 2);
}

KTEST_CASE(ktl_views_filter) {
    int arr[6] = {1, 2, 3, 4, 5, 6};
    int sum    = 0;
    int n      = 0;
    for (int v : ktl::span(arr) | ktl::views::filter([](int x) { return x % 2 == 0; })) {
        sum += v;
        ++n;
    }
    KTEST_EXPECT_ALL(n == 3, sum == 12);

    // No matches -> empty.
    int none = 0;
    for (int v : ktl::span(arr) | ktl::views::filter([](int) { return false; })) { none += v; }
    KTEST_EXPECT_EQUAL(none, 0);
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
    KTEST_EXPECT_ALL(k == 2, seen_i[0] == (size_t)3, seen_v[0] == 40, seen_i[1] == (size_t)4, seen_v[1] == 50);
}
