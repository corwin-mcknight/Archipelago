#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/testing/tracking_value.h>

#include <ktl/tuple>

using namespace kernel::testing;

KTEST(ktl_tuple_construct_and_get, "ktl/tuple") {
    ktl::tuple<int, char, bool> t{7, 'a', true};
    KTEST_EXPECT_TRUE(ktl::get<0>(t) == 7);
    KTEST_EXPECT_TRUE(ktl::get<1>(t) == 'a');
    KTEST_EXPECT_TRUE(ktl::get<2>(t) == true);

    ktl::get<0>(t) = 9;
    KTEST_EXPECT_TRUE(ktl::get<0>(t) == 9);

    ktl::tuple<int, int> def;
    KTEST_EXPECT_ALL(ktl::get<0>(def) == 0, ktl::get<1>(def) == 0);
}

KTEST(ktl_tuple_traits, "ktl/tuple") {
    using T = ktl::tuple<int, char, bool>;
    static_assert(ktl::tuple_size_v<T> == 3);
    static_assert(ktl::is_same_v<ktl::tuple_element_t<0, T>, int>);
    static_assert(ktl::is_same_v<ktl::tuple_element_t<2, T>, bool>);
    static_assert(ktl::tuple_size_v<ktl::tuple<>> == 0);
    KTEST_EXPECT_TRUE(true);
}

KTEST(ktl_tuple_make_and_decay, "ktl/tuple") {
    int x   = 5;
    auto tp = ktl::make_tuple(x, 'z', 2u);
    static_assert(ktl::is_same_v<ktl::tuple_element_t<0, decltype(tp)>, int>);
    static_assert(ktl::is_same_v<ktl::tuple_element_t<2, decltype(tp)>, unsigned int>);
    KTEST_EXPECT_ALL(ktl::get<0>(tp) == 5, ktl::get<1>(tp) == 'z', ktl::get<2>(tp) == 2u);
}

KTEST(ktl_tuple_equality, "ktl/tuple") {
    ktl::tuple<int, int> a{1, 2};
    ktl::tuple<int, int> b{1, 2};
    ktl::tuple<int, int> c{1, 3};
    KTEST_EXPECT_TRUE(a == b);
    KTEST_EXPECT_TRUE(a != c);

    ktl::tuple<> e1;
    ktl::tuple<> e2;
    KTEST_EXPECT_TRUE(e1 == e2);
}

KTEST(ktl_tuple_tie_and_move, "ktl/tuple") {
    int a          = 0;
    char b         = 0;
    ktl::tie(a, b) = ktl::make_tuple(11, 'q');
    KTEST_EXPECT_ALL(a == 11, b == 'q');

    ktl::tuple<tracking_value, int> src{tracking_value{42}, 5};
    ktl::tuple<tracking_value, int> dst{ktl::move(src)};
    KTEST_EXPECT_ALL(ktl::get<0>(dst).value == 42, ktl::get<0>(dst).move_observed, ktl::get<1>(dst) == 5);
}

KTEST(ktl_tuple_structured_binding, "ktl/tuple") {
    ktl::tuple<int, char, bool> t{7, 'a', true};

    // Bind by reference: writes go back to the original tuple.
    auto& [i, c, b] = t;
    KTEST_EXPECT_ALL(i == 7, c == 'a', b == true);
    i = 9;
    KTEST_EXPECT_TRUE(ktl::get<0>(t) == 9);

    // Bind a copy of a prvalue.
    auto [x, y, z] = ktl::make_tuple(1, 2u, 'q');
    KTEST_EXPECT_ALL(x == 1, y == 2u, z == 'q');

    // Const reference binding.
    const ktl::tuple<int, int> ct{4, 5};
    const auto& [p, q] = ct;
    KTEST_EXPECT_ALL(p == 4, q == 5);
}

#endif
