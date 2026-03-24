#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <ktl/stack>
#include <ktl/vector>
#include <ktl/utility>

using namespace kernel::testing;

namespace {

struct tracking_value {
    int value          = 0;
    bool move_observed = false;

    tracking_value()   = default;
    explicit tracking_value(int v) : value(v) {}

    tracking_value(const tracking_value&)            = default;
    tracking_value& operator=(const tracking_value&) = default;

    tracking_value(tracking_value&& other) noexcept : value(other.value), move_observed(true) {
        other.value         = -1;
        other.move_observed = true;
    }

    tracking_value& operator=(tracking_value&& other) noexcept {
        value               = other.value;
        move_observed       = true;
        other.value         = -1;
        other.move_observed = true;
        return *this;
    }
};

}  // namespace

KTEST(ktl_stack_push_pop_top, "ktl/stack") {
    ktl::stack<int> values;

    KTEST_EXPECT_TRUE(values.empty());
    KTEST_EXPECT_TRUE(values.size() == 0);

    KTEST_REQUIRE_TRUE(values.push(1));
    KTEST_REQUIRE_TRUE(values.push(2));
    KTEST_REQUIRE_TRUE(values.push(3));

    KTEST_EXPECT_TRUE(values.size() == 3);

    auto top = values.top();
    KTEST_REQUIRE_TRUE(top.has_value());
    KTEST_EXPECT_EQUAL(top.value(), 3);

    auto popped = values.pop();
    KTEST_REQUIRE_TRUE(popped.has_value());
    KTEST_EXPECT_EQUAL(popped.value(), 3);

    KTEST_EXPECT_TRUE(values.size() == 2);

    values.clear();
    KTEST_EXPECT_TRUE(values.empty());
}

KTEST(ktl_stack_move_and_alternate_container, "ktl/stack") {
    ktl::stack<tracking_value> default_stack;
    tracking_value val{42};

    KTEST_REQUIRE_TRUE(default_stack.push(tracking_value{7}));
    KTEST_REQUIRE_TRUE(default_stack.emplace(ktl::move(val)));
    KTEST_EXPECT_TRUE(val.move_observed);
    KTEST_EXPECT_EQUAL(val.value, -1);

    auto top = default_stack.top();
    KTEST_REQUIRE_TRUE(top.has_value());
    KTEST_EXPECT_EQUAL(top.value().value, 42);

    auto moved = default_stack.pop();
    KTEST_REQUIRE_TRUE(moved.has_value());
    KTEST_EXPECT_EQUAL(moved.value().value, 42);
    KTEST_EXPECT_TRUE(moved.value().move_observed);

    ktl::stack<int, ktl::vector<int>> vector_backed;
    KTEST_REQUIRE_TRUE(vector_backed.push(10));
    KTEST_REQUIRE_TRUE(vector_backed.push(20));

    auto vector_top = vector_backed.top();
    KTEST_REQUIRE_TRUE(vector_top.has_value());
    KTEST_EXPECT_EQUAL(vector_top.value(), 20);

    auto vector_pop = vector_backed.pop();
    KTEST_REQUIRE_TRUE(vector_pop.has_value());
    KTEST_EXPECT_EQUAL(vector_pop.value(), 20);

    KTEST_EXPECT_TRUE(vector_backed.size() == 1);
}

#endif
