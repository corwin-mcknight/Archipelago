#include <kernel/testing/testing.h>
#include <stddef.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/testing/tracking_value.h>

#include <ktl/deque>
#include <ktl/maybe>
#include <ktl/utility>

using namespace kernel::testing;

namespace {

template <size_t Max> struct simple_int_deque {
    int data[Max] = {};
    size_t head   = 0;
    size_t count  = 0;

    void push_back(int value) { data[(head + count++) % Max] = value; }
    void push_front(int value) {
        head       = (head + Max - 1) % Max;
        data[head] = value;
        ++count;
    }
    int pop_front() {
        int v = data[head];
        head  = (head + 1) % Max;
        --count;
        return v;
    }
    int pop_back() {
        int v = data[(head + count - 1) % Max];
        --count;
        return v;
    }
    int at(size_t index) const { return data[(head + index) % Max]; }
    bool empty() const { return count == 0; }
    size_t size() const { return count; }
};

}  // namespace

KTEST(ktl_deque_push_front_back_order, "ktl/deque") {
    ktl::deque<int> dq;
    KTEST_REQUIRE_TRUE(dq.empty());

    KTEST_REQUIRE_TRUE(dq.push_back(2));
    KTEST_REQUIRE_TRUE(dq.push_front(1));
    KTEST_REQUIRE_TRUE(dq.push_back(3));
    KTEST_REQUIRE_TRUE(dq.emplace_front(0));

    KTEST_EXPECT_TRUE(dq.size() == 4);
    KTEST_EXPECT_EQUAL(dq[0], 0);
    KTEST_EXPECT_EQUAL(dq[1], 1);
    KTEST_EXPECT_EQUAL(dq[2], 2);
    KTEST_EXPECT_EQUAL(dq[3], 3);

    KTEST_EXPECT_VALUE(dq.front(), 0);
    KTEST_EXPECT_VALUE(dq.back(), 3);
}

KTEST(ktl_deque_pop_front_back, "ktl/deque") {
    ktl::deque<int> dq;
    for (int i = 0; i < 5; ++i) { KTEST_REQUIRE_TRUE(dq.push_back(i)); }

    KTEST_EXPECT_VALUE(dq.pop_front(), 0);
    KTEST_EXPECT_TRUE(dq.size() == 4);

    KTEST_EXPECT_VALUE(dq.pop_back(), 4);
    KTEST_EXPECT_TRUE(dq.size() == 3);

    KTEST_EXPECT_EQUAL(dq[0], 1);
    KTEST_EXPECT_EQUAL(dq[1], 2);
    KTEST_EXPECT_EQUAL(dq[2], 3);

    dq.clear();
    KTEST_EXPECT_TRUE(dq.empty());
    KTEST_EXPECT_FALSE(dq.pop_front().has_value());
}

KTEST(ktl_deque_move_semantics, "ktl/deque") {
    ktl::deque<tracking_value> dq;

    tracking_value front_value{10};
    tracking_value back_value{20};

    KTEST_REQUIRE_TRUE(dq.emplace_front(ktl::move(front_value)));
    KTEST_EXPECT_ALL(front_value.move_observed, front_value.value == -1);

    KTEST_REQUIRE_TRUE(dq.emplace_back(ktl::move(back_value)));
    KTEST_EXPECT_ALL(back_value.move_observed, back_value.value == -1);

    KTEST_EXPECT_ALL(dq.size() == 2, dq[0].value == 10, dq[0].move_observed, dq[1].value == 20, dq[1].move_observed);

    KTEST_REQUIRE_VALUE(moved_out, dq.pop_front());
    KTEST_EXPECT_ALL(moved_out.value == 10, moved_out.move_observed);

    KTEST_REQUIRE_VALUE(remaining, dq.front());
    KTEST_EXPECT_EQUAL(remaining.value, 20);
}

KTEST(ktl_deque_iterator_and_reserve, "ktl/deque") {
    ktl::deque<int> dq;
    KTEST_REQUIRE_TRUE(dq.reserve(64));
    KTEST_EXPECT_TRUE(dq.capacity() >= 64);

    for (int i = 0; i < 40; ++i) { KTEST_REQUIRE_TRUE(dq.push_back(i)); }

    int expected = 0;
    for (auto it = dq.begin(); it != dq.end(); ++it) {
        KTEST_EXPECT_EQUAL(*it, expected);
        ++expected;
    }
    KTEST_EXPECT_EQUAL(expected, 40);

    auto mutable_begin                        = dq.begin();
    ktl::deque<int>::const_iterator converted = mutable_begin;
    KTEST_EXPECT_ALL(converted == dq.begin(), dq.begin() == converted);
    KTEST_EXPECT_EQUAL(converted[10], 10);
    KTEST_EXPECT_EQUAL(dq.begin()[15], 15);

    const auto& const_ref = dq;
    int index             = 0;
    for (auto it = const_ref.begin(); it != const_ref.end(); ++it) {
        KTEST_EXPECT_EQUAL(*it, index);
        ++index;
    }

    KTEST_EXPECT_ALL(const_ref.cbegin() == const_ref.begin(), const_ref.cend() == const_ref.end());
    KTEST_EXPECT_EQUAL(static_cast<size_t>(dq.end() - dq.begin()), dq.size());
    KTEST_EXPECT_EQUAL(static_cast<size_t>(const_ref.cend() - dq.begin()), const_ref.size());
    KTEST_EXPECT_EQUAL(static_cast<size_t>(dq.begin() - const_ref.cbegin()), 0u);
    KTEST_EXPECT_EQUAL(*(dq.begin() + 5), 5);
    KTEST_EXPECT_EQUAL(*(dq.begin() + 20), 20);
}

KTEST(ktl_deque_stress_front_back_mixed, "ktl/deque") {
    ktl::deque<int> dq;

    constexpr size_t total_ops = 384;
    simple_int_deque<total_ops> model;

    for (size_t i = 0; i < total_ops; ++i) {
        int value = static_cast<int>(i);
        if ((i % 3) == 0) {
            KTEST_REQUIRE_TRUE(dq.push_back(value));
            model.push_back(value);
        } else if ((i % 3) == 1) {
            KTEST_REQUIRE_TRUE(dq.push_front(value));
            model.push_front(value);
        } else {
            KTEST_REQUIRE_TRUE(dq.push_back(value));
            model.push_back(value);
            KTEST_EXPECT_VALUE(dq.pop_front(), model.pop_front());
        }
    }

    KTEST_EXPECT_EQUAL(dq.size(), model.size());

    size_t index = 0;
    for (auto it = dq.begin(); it != dq.end(); ++it) {
        KTEST_EXPECT_TRUE(index < model.size());
        KTEST_EXPECT_EQUAL(*it, model.at(index));
        ++index;
    }
    KTEST_EXPECT_EQUAL(index, model.size());

    bool pop_front_next = true;
    while (!model.empty()) {
        if (pop_front_next) {
            KTEST_EXPECT_VALUE(dq.pop_front(), model.pop_front());
        } else {
            KTEST_EXPECT_VALUE(dq.pop_back(), model.pop_back());
        }
        pop_front_next = !pop_front_next;
    }
    KTEST_EXPECT_TRUE(dq.empty());
}

KTEST(ktl_deque_stress_block_reuse, "ktl/deque") {
    ktl::deque<int> dq;

    constexpr size_t passes     = 64;
    constexpr size_t block_size = 128;

    for (size_t pass = 0; pass < passes; ++pass) {
        simple_int_deque<block_size> model;

        for (size_t i = 0; i < block_size; ++i) {
            int value = static_cast<int>(pass * block_size + i);
            switch (i % 4) {
                case 0:
                case 2:
                    KTEST_REQUIRE_TRUE(dq.push_back(value));
                    model.push_back(value);
                    break;
                default:
                    KTEST_REQUIRE_TRUE(dq.push_front(value));
                    model.push_front(value);
                    break;
            }
        }

        KTEST_EXPECT_EQUAL(dq.size(), model.size());

        for (size_t i = 0; i < block_size / 2; ++i) {
            KTEST_EXPECT_VALUE(dq.pop_front(), model.pop_front());
            KTEST_EXPECT_VALUE(dq.pop_back(), model.pop_back());
        }
        KTEST_EXPECT_TRUE(dq.empty());
    }
}

KTEST(ktl_deque_range_for, "ktl/deque") {
    ktl::deque<int> dq;
    for (int i = 0; i < 16; ++i) {
        if ((i % 2) == 0) {
            KTEST_REQUIRE_TRUE(dq.push_back(i));
        } else {
            KTEST_REQUIRE_TRUE(dq.push_front(i));
        }
    }

    int count     = 0;
    int total_sum = 0;
    for (auto& value : dq) {
        value += 1;
        total_sum += value;
        ++count;
    }
    KTEST_EXPECT_EQUAL(count, static_cast<int>(dq.size()));

    const auto& const_ref = dq;
    int verify_sum        = 0;
    for (const auto& value : const_ref) { verify_sum += value; }
    KTEST_EXPECT_EQUAL(verify_sum, total_sum);
}

#endif  // CONFIG_KERNEL_TESTING
