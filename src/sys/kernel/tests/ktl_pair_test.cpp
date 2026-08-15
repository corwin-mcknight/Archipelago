#include <kernel/testing/testing.h>
#include <kernel/testing/tracking_value.h>

#include <ktl/utility>

using namespace kernel::testing;

KTEST_MODULE("ktl/pair");

KTEST_CASE(ktl_pair_value_semantics) {
    ktl::pair<int, char> p{7, 'a'};
    KTEST_EXPECT_ALL(p.first == 7, p.second == 'a');

    ktl::pair<int, char> def;
    KTEST_EXPECT_TRUE(def.first == 0);

    ktl::pair<int, int> a{1, 2};
    ktl::pair<int, int> b{1, 2};
    ktl::pair<int, int> c{1, 3};
    KTEST_EXPECT_ALL(a == b, a != c);

    // Structured bindings via public members.
    auto [f, s] = a;
    KTEST_EXPECT_ALL(f == 1, s == 2);

    // Reference member aliases external storage (write-through).
    int x = 5;
    ktl::pair<size_t, int&> r((size_t)7, x);
    KTEST_EXPECT_EQUAL(r.first, (size_t)7);
    r.second = 9;
    KTEST_EXPECT_EQUAL(x, 9);
}

KTEST_CASE(ktl_pair_move) {
    ktl::pair<tracking_value, int> src{tracking_value{42}, 5};
    ktl::pair<tracking_value, int> dst{ktl::move(src)};
    KTEST_EXPECT_ALL(dst.first.value == 42, dst.first.move_observed, dst.second == 5);
}
