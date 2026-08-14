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

    // make_pair decays its arguments.
    auto made = ktl::make_pair(1, 2u);
    KTEST_EXPECT_ALL(made.first == 1, made.second == 2u);
    static_assert(ktl::is_same<decltype(made)::second_type, unsigned int>::value);

    ktl::pair<int, int> a{1, 2};
    ktl::pair<int, int> b{1, 2};
    ktl::pair<int, int> c{1, 3};
    KTEST_EXPECT_ALL(a == b, a != c);

    // Structured bindings via public members, and get<I> on pair.
    auto [f, s] = a;
    KTEST_EXPECT_ALL(f == 1, s == 2, ktl::get<0>(a) == 1, ktl::get<1>(a) == 2);

    // Reference member aliases external storage (write-through).
    int x = 5;
    ktl::pair<size_t, int&> r((size_t)7, x);
    KTEST_EXPECT_EQUAL(r.first, (size_t)7);
    r.second = 9;
    KTEST_EXPECT_EQUAL(x, 9);
}

KTEST_CASE(ktl_pair_move_and_swap) {
    ktl::pair<tracking_value, int> src{tracking_value{42}, 5};
    ktl::pair<tracking_value, int> dst{ktl::move(src)};
    KTEST_EXPECT_ALL(dst.first.value == 42, dst.first.move_observed, dst.second == 5);

    ktl::pair<int, int> x{1, 2};
    ktl::pair<int, int> y{3, 4};
    x.swap(y);
    KTEST_EXPECT_ALL(x.first == 3, x.second == 4, y.first == 1, y.second == 2);
}
