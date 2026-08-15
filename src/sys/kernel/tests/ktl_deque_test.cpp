#include <kernel/testing/testing.h>
#include <kernel/testing/tracking_value.h>
#include <stddef.h>

#include <ktl/deque>
#include <ktl/maybe>
#include <ktl/utility>

using namespace kernel::testing;

KTEST_MODULE("ktl/deque");

KTEST_CASE(ktl_deque_fifo_order) {
    ktl::deque<int> dq;
    KTEST_REQUIRE_TRUE(dq.empty());

    for (int i = 0; i < 4; ++i) { KTEST_REQUIRE_TRUE(dq.push_back(i)); }
    KTEST_EXPECT_ALL(dq.size() == 4, !dq.empty());
    KTEST_EXPECT_VALUE(dq.back(), 3);

    for (int i = 0; i < 4; ++i) { KTEST_EXPECT_VALUE(dq.pop_front(), i); }
    KTEST_EXPECT_TRUE(dq.empty());
}

KTEST_CASE(ktl_deque_pop_front_back) {
    ktl::deque<int> dq;
    for (int i = 0; i < 5; ++i) { KTEST_REQUIRE_TRUE(dq.push_back(i)); }

    KTEST_EXPECT_VALUE(dq.pop_front(), 0);
    KTEST_EXPECT_TRUE(dq.size() == 4);

    KTEST_EXPECT_VALUE(dq.pop_back(), 4);
    KTEST_EXPECT_TRUE(dq.size() == 3);

    dq.clear();
    KTEST_EXPECT_ALL(dq.empty(), !dq.pop_front().has_value(), !dq.pop_back().has_value());
}

KTEST_CASE(ktl_deque_move_semantics) {
    ktl::deque<tracking_value> dq;

    tracking_value back_value{20};
    KTEST_REQUIRE_TRUE(dq.emplace_back(ktl::move(back_value)));
    KTEST_EXPECT_ALL(back_value.move_observed, back_value.value == -1);

    KTEST_REQUIRE_VALUE(moved_out, dq.pop_front());
    KTEST_EXPECT_ALL(moved_out.value == 20, moved_out.move_observed);
    KTEST_EXPECT_TRUE(dq.empty());
}

KTEST_CASE(ktl_deque_reserve_and_multi_block_fifo) {
    ktl::deque<int> dq;
    KTEST_REQUIRE_TRUE(dq.reserve(64));

    // Spans several 16-element blocks; order must survive the block walks.
    for (int i = 0; i < 40; ++i) { KTEST_REQUIRE_TRUE(dq.push_back(i)); }
    KTEST_EXPECT_EQUAL(dq.size(), (size_t)40);
    for (int i = 0; i < 40; ++i) { KTEST_EXPECT_VALUE(dq.pop_front(), i); }
    KTEST_EXPECT_TRUE(dq.empty());
}

KTEST_CASE(ktl_deque_stress_mixed_pops) {
    // FIFO pushes drained from both ends against a plain ring-buffer model.
    constexpr size_t total = 384;
    int model[total];
    size_t head = 0, tail = 0;

    ktl::deque<int> dq;
    for (size_t i = 0; i < total; ++i) {
        int value = static_cast<int>(i);
        KTEST_REQUIRE_TRUE(dq.push_back(value));
        model[tail++] = value;
        if ((i % 3) == 2) { KTEST_EXPECT_VALUE(dq.pop_front(), model[head++]); }
    }
    KTEST_EXPECT_EQUAL(dq.size(), tail - head);

    bool pop_front_next = true;
    while (head < tail) {
        if (pop_front_next) {
            KTEST_EXPECT_VALUE(dq.pop_front(), model[head++]);
        } else {
            KTEST_EXPECT_VALUE(dq.pop_back(), model[--tail]);
        }
        pop_front_next = !pop_front_next;
    }
    KTEST_EXPECT_TRUE(dq.empty());
}

KTEST_CASE(ktl_deque_drain_to_empty_from_front) {
    // Draining every element through pop_front is the scheduler run queue's access pattern, and it
    // is the case that leaves every block empty at once. Block compaction has to terminate there
    // rather than shifting empty blocks forever; a regression hangs this case instead of failing.
    constexpr int element_count = 40;

    ktl::deque<int> dq;
    for (int i = 0; i < element_count; ++i) { KTEST_REQUIRE_TRUE(dq.push_back(i)); }
    for (int i = 0; i < element_count; ++i) { KTEST_EXPECT_VALUE(dq.pop_front(), i); }

    KTEST_EXPECT_ALL(dq.empty(), dq.size() == static_cast<size_t>(0), !dq.pop_front().has_value());

    // The emptied blocks are kept as reserve, so refilling must still land elements in order.
    for (int i = 0; i < element_count; ++i) { KTEST_REQUIRE_TRUE(dq.push_back(i * 2)); }
    KTEST_REQUIRE_EQUAL(dq.size(), static_cast<size_t>(element_count));
    for (int i = 0; i < element_count; ++i) { KTEST_EXPECT_VALUE(dq.pop_front(), i * 2); }
    KTEST_EXPECT_TRUE(dq.empty());
}

KTEST_CASE(ktl_deque_drain_to_empty_alternating) {
    // Same compaction path reached with the front and back ends emptying against each other.
    constexpr int element_count = 40;

    ktl::deque<int> dq;
    for (int i = 0; i < element_count; ++i) { KTEST_REQUIRE_TRUE(dq.push_back(i)); }

    int low  = 0;
    int high = element_count - 1;
    while (low <= high) {
        KTEST_EXPECT_VALUE(dq.pop_front(), low);
        ++low;
        if (low > high) { break; }
        KTEST_EXPECT_VALUE(dq.pop_back(), high);
        --high;
    }

    KTEST_EXPECT_TRUE(dq.empty());
    KTEST_REQUIRE_TRUE(dq.push_back(99));
    KTEST_EXPECT_VALUE(dq.pop_back(), 99);
}
