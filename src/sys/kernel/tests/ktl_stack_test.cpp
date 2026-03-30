#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/testing/tracking_value.h>

#include <ktl/stack>
#include <ktl/utility>
#include <ktl/vector>

using namespace kernel::testing;

KTEST(ktl_stack_push_pop_top, "ktl/stack") {
    ktl::stack<int> values;
    KTEST_EXPECT_ALL(values.empty(), values.size() == 0);

    KTEST_REQUIRE_TRUE(values.push(1));
    KTEST_REQUIRE_TRUE(values.push(2));
    KTEST_REQUIRE_TRUE(values.push(3));
    KTEST_EXPECT_TRUE(values.size() == 3);

    KTEST_EXPECT_VALUE(values.top(), 3);
    KTEST_EXPECT_VALUE(values.pop(), 3);
    KTEST_EXPECT_TRUE(values.size() == 2);

    values.clear();
    KTEST_EXPECT_TRUE(values.empty());
}

KTEST(ktl_stack_move_and_alternate_container, "ktl/stack") {
    ktl::stack<tracking_value> default_stack;
    tracking_value val{42};

    KTEST_REQUIRE_TRUE(default_stack.push(tracking_value{7}));
    KTEST_REQUIRE_TRUE(default_stack.emplace(ktl::move(val)));
    KTEST_EXPECT_ALL(val.move_observed, val.value == -1);

    KTEST_REQUIRE_VALUE(top, default_stack.top());
    KTEST_EXPECT_EQUAL(top.value, 42);

    KTEST_REQUIRE_VALUE(moved, default_stack.pop());
    KTEST_EXPECT_ALL(moved.value == 42, moved.move_observed);

    ktl::stack<int, ktl::vector<int>> vector_backed;
    KTEST_REQUIRE_TRUE(vector_backed.push(10));
    KTEST_REQUIRE_TRUE(vector_backed.push(20));

    KTEST_EXPECT_VALUE(vector_backed.top(), 20);
    KTEST_EXPECT_VALUE(vector_backed.pop(), 20);
    KTEST_EXPECT_TRUE(vector_backed.size() == 1);
}

#endif
